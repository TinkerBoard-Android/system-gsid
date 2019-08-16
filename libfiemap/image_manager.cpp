//
// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <libfiemap/image_manager.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <ext4_utils/ext4_utils.h>
#include <fs_mgr/file_wait.h>
#include <fs_mgr_dm_linear.h>
#include <libdm/loop_control.h>
#include <libfiemap/split_fiemap_writer.h>

#include "metadata.h"
#include "utility.h"

namespace android {
namespace fiemap {

using namespace std::literals;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetLinear;
using android::dm::LoopControl;
using android::fs_mgr::CreateLogicalPartition;
using android::fs_mgr::CreateLogicalPartitionParams;
using android::fs_mgr::DestroyLogicalPartition;
using android::fs_mgr::GetPartitionName;

static constexpr char kTestImageMetadataDir[] = "/metadata/gsi/test";

std::unique_ptr<IImageManager> __attribute__((weak))
IImageManager::Open(const std::string& dir_prefix, const std::chrono::milliseconds& timeout_ms) {
    (void)timeout_ms;
    return ImageManager::Open(dir_prefix);
}

std::unique_ptr<ImageManager> ImageManager::Open(const std::string& dir_prefix) {
    auto metadata_dir = "/metadata/gsi/" + dir_prefix;
    auto data_dir = "/data/gsi/" + dir_prefix;
    return Open(metadata_dir, data_dir);
}

std::unique_ptr<ImageManager> ImageManager::Open(const std::string& metadata_dir,
                                                 const std::string& data_dir) {
    return std::unique_ptr<ImageManager>(new ImageManager(metadata_dir, data_dir));
}

ImageManager::ImageManager(const std::string& metadata_dir, const std::string& data_dir)
    : metadata_dir_(metadata_dir), data_dir_(data_dir) {
    partition_opener_ = std::make_unique<android::fs_mgr::PartitionOpener>();
}

std::string ImageManager::GetImageHeaderPath(const std::string& name) {
    return JoinPaths(data_dir_, name) + ".img";
}

// The status file has one entry per line, with each entry formatted as one of:
//   dm:<name>
//   loop:<path>
//
// This simplifies the process of tearing down a mapping, since we can simply
// unmap each entry in the order it appears.
std::string ImageManager::GetStatusFilePath(const std::string& image_name) {
    return JoinPaths(metadata_dir_, image_name) + ".status";
}

static std::string GetStatusPropertyName(const std::string& image_name) {
    // Note: we don't prefix |image_name|, because CreateLogicalPartition won't
    // prefix the name either. There are no plans to change this at the moment,
    // consumers of the image API must take care to use globally-unique image
    // names.
    return "gsid.mapped_image." + image_name;
}

void ImageManager::set_partition_opener(std::unique_ptr<IPartitionOpener>&& opener) {
    partition_opener_ = std::move(opener);
}

bool ImageManager::IsImageMapped(const std::string& image_name) {
    auto prop_name = GetStatusPropertyName(image_name);
    if (android::base::GetProperty(prop_name, "").empty()) {
        // If mapped in first-stage init, the dm-device will exist but not the
        // property.
        auto& dm = DeviceMapper::Instance();
        return dm.GetState(image_name) != DmDeviceState::INVALID;
    }
    return true;
}

bool ImageManager::PartitionExists(const std::string& name) {
    if (!MetadataExists(metadata_dir_)) {
        return false;
    }
    auto metadata = OpenMetadata(metadata_dir_);
    if (!metadata) {
        return false;
    }
    return !!FindPartition(*metadata.get(), name);
}

bool ImageManager::BackingImageExists(const std::string& name) {
    auto header_file = GetImageHeaderPath(name);
    return access(header_file.c_str(), F_OK) == 0;
}

bool ImageManager::CreateBackingImage(const std::string& name, uint64_t size, int flags) {
    return CreateBackingImage(name, size, flags, nullptr);
}

bool ImageManager::CreateBackingImage(const std::string& name, uint64_t size, int flags,
                                      std::function<bool(uint64_t, uint64_t)>&& on_progress) {
    auto data_path = GetImageHeaderPath(name);
    auto fw = SplitFiemap::Create(data_path, size, 0, on_progress);
    if (!fw) {
        return false;
    }

    // Except for testing, we do not allow persisting metadata that references
    // device-mapper devices. It just doesn't make sense, because the device
    // numbering may change on reboot. We allow it for testing since the images
    // are not meant to survive reboot. Outside of tests, this can only happen
    // if device-mapper is stacked in some complex way not supported by
    // FiemapWriter.
    auto device_path = GetDevicePathForFile(fw.get());
    if (android::base::StartsWith(device_path, "/dev/block/dm-") &&
        !android::base::StartsWith(metadata_dir_, kTestImageMetadataDir)) {
        LOG(ERROR) << "Cannot persist images against device-mapper device: " << device_path;

        fw = {};
        SplitFiemap::RemoveSplitFiles(data_path);
        return false;
    }

    bool readonly = !!(flags & CREATE_IMAGE_READONLY);
    if (!UpdateMetadata(metadata_dir_, name, fw.get(), size, readonly)) {
        return false;
    }

    if (flags & CREATE_IMAGE_ZERO_FILL) {
        if (!ZeroFillNewImage(name)) {
            DeleteBackingImage(name);
            return false;
        }
    }
    return true;
}

bool ImageManager::ZeroFillNewImage(const std::string& name) {
    auto data_path = GetImageHeaderPath(name);

    // See the comment in MapImageDevice() about how this works.
    std::string block_device;
    bool can_use_devicemapper;
    if (!FiemapWriter::GetBlockDeviceForFile(data_path, &block_device, &can_use_devicemapper)) {
        LOG(ERROR) << "Could not determine block device for " << data_path;
        return false;
    }

    if (!can_use_devicemapper) {
        // We've backed with loop devices, and since we store files in an
        // unencrypted folder, the initial zeroes we wrote will suffice.
        return true;
    }

    // data is dm-crypt, or FBE + dm-default-key. This means the zeroes written
    // by libfiemap were encrypted, so we need to map the image in and correct
    // this.
    auto device = MappedDevice::Open(this, 10s, name);
    if (!device) {
        return false;
    }

    static constexpr size_t kChunkSize = 4096;
    std::string zeroes(kChunkSize, '\0');

    uint64_t remaining = get_block_device_size(device->fd());
    if (!remaining) {
        PLOG(ERROR) << "Could not get block device size for " << device->path();
        return false;
    }
    while (remaining) {
        uint64_t to_write = std::min(static_cast<uint64_t>(zeroes.size()), remaining);
        if (!android::base::WriteFully(device->fd(), zeroes.data(),
                                       static_cast<size_t>(to_write))) {
            PLOG(ERROR) << "write failed: " << device->path();
            return false;
        }
        remaining -= to_write;
    }
    return true;
}

bool ImageManager::DeleteBackingImage(const std::string& name) {
    // For dm-linear devices sitting on top of /data, we cannot risk deleting
    // the file. The underlying blocks could be reallocated by the filesystem.
    if (IsImageMapped(name)) {
        LOG(ERROR) << "Backing image " << name << " is currently mapped to a block device";
        return false;
    }

    std::string message;
    auto header_file = GetImageHeaderPath(name);
    if (!SplitFiemap::RemoveSplitFiles(header_file, &message)) {
        // This is fatal, because we don't want to leave these files dangling.
        LOG(ERROR) << "Error removing image " << name << ": " << message;
        return false;
    }

    auto status_file = GetStatusFilePath(name);
    if (!android::base::RemoveFileIfExists(status_file)) {
        LOG(ERROR) << "Error removing " << status_file << ": " << message;
    }
    return RemoveImageMetadata(metadata_dir_, name);
}

// Create a block device for an image file, using its extents in its
// lp_metadata.
bool ImageManager::MapWithDmLinear(const IPartitionOpener& opener, const std::string& name,
                                   const std::chrono::milliseconds& timeout_ms, std::string* path) {
    // :TODO: refresh extents in metadata file until f2fs is fixed.
    auto metadata = OpenMetadata(metadata_dir_);
    if (!metadata) {
        return false;
    }

    auto super = android::fs_mgr::GetMetadataSuperBlockDevice(*metadata.get());
    auto block_device = android::fs_mgr::GetBlockDevicePartitionName(*super);

    CreateLogicalPartitionParams params = {
            .block_device = block_device,
            .metadata = metadata.get(),
            .partition_name = name,
            .force_writable = true,
            .timeout_ms = timeout_ms,
            .partition_opener = &opener,
    };
    if (!CreateLogicalPartition(params, path)) {
        LOG(ERROR) << "Error creating device-mapper node for image " << name;
        return false;
    }

    auto status_string = "dm:" + name;
    auto status_file = GetStatusFilePath(name);
    if (!android::base::WriteStringToFile(status_string, status_file)) {
        PLOG(ERROR) << "Could not write status file: " << status_file;
        DestroyLogicalPartition(name);
        return false;
    }
    return true;
}

// Helper to create a loop device for a file.
static bool CreateLoopDevice(LoopControl& control, const std::string& file,
                             const std::chrono::milliseconds& timeout_ms, std::string* path) {
    static constexpr int kOpenFlags = O_RDWR | O_NOFOLLOW | O_CLOEXEC;
    android::base::unique_fd file_fd(open(file.c_str(), kOpenFlags));
    if (file_fd < 0) {
        PLOG(ERROR) << "Could not open file: " << file;
        return false;
    }
    if (!control.Attach(file_fd, timeout_ms, path)) {
        LOG(ERROR) << "Could not create loop device for: " << file;
        return false;
    }
    LOG(INFO) << "Created loop device " << *path << " for file " << file;
    return true;
}

class AutoDetachLoopDevices final {
  public:
    AutoDetachLoopDevices(LoopControl& control, const std::vector<std::string>& devices)
        : control_(control), devices_(devices), commit_(false) {}

    ~AutoDetachLoopDevices() {
        if (commit_) return;
        for (const auto& device : devices_) {
            control_.Detach(device);
        }
    }

    void Commit() { commit_ = true; }

  private:
    LoopControl& control_;
    const std::vector<std::string>& devices_;
    bool commit_;
};

// If an image is stored across multiple files, this takes a list of loop
// devices and joins them together using device-mapper.
bool ImageManager::MapWithLoopDeviceList(const std::vector<std::string>& device_list,
                                         const std::string& name,
                                         const std::chrono::milliseconds& timeout_ms,
                                         std::string* path) {
    auto metadata = OpenMetadata(metadata_dir_);
    if (!metadata) {
        return false;
    }
    auto partition = FindPartition(*metadata.get(), name);
    if (!partition) {
        LOG(ERROR) << "Could not find image in metadata: " << name;
        return false;
    }

    // Since extent lengths are in sector units, the size should be a multiple
    // of the sector size.
    uint64_t partition_size = GetPartitionSize(*metadata.get(), *partition);
    if (partition_size % LP_SECTOR_SIZE != 0) {
        LOG(ERROR) << "Partition size not sector aligned: " << name << ", " << partition_size
                   << " bytes";
        return false;
    }

    DmTable table;

    uint64_t start_sector = 0;
    uint64_t sectors_needed = partition_size / LP_SECTOR_SIZE;
    for (const auto& block_device : device_list) {
        // The final block device must be == partition_size, otherwise we
        // can't find the AVB footer on verified partitions.
        static constexpr int kOpenFlags = O_RDWR | O_NOFOLLOW | O_CLOEXEC;
        unique_fd fd(open(block_device.c_str(), kOpenFlags));
        if (fd < 0) {
            PLOG(ERROR) << "Open failed: " << block_device;
            return false;
        }

        uint64_t file_size = get_block_device_size(fd);
        uint64_t file_sectors = file_size / LP_SECTOR_SIZE;
        uint64_t segment_size = std::min(file_sectors, sectors_needed);

        table.Emplace<DmTargetLinear>(start_sector, segment_size, block_device, 0);

        start_sector += segment_size;
        sectors_needed -= segment_size;
        if (sectors_needed == 0) {
            break;
        }
    }

    auto& dm = DeviceMapper::Instance();
    if (!dm.CreateDevice(name, table, path, timeout_ms)) {
        LOG(ERROR) << "Could not create device-mapper device over loop set";
        return false;
    }

    // Build the status file.
    std::vector<std::string> lines;
    lines.emplace_back("dm:" + name);
    for (const auto& block_device : device_list) {
        lines.emplace_back("loop:" + block_device);
    }
    auto status_message = android::base::Join(lines, "\n");
    auto status_file = GetStatusFilePath(name);
    if (!android::base::WriteStringToFile(status_message, status_file)) {
        PLOG(ERROR) << "Write failed: " << status_file;
        dm.DeleteDevice(name);
        return false;
    }
    return true;
}

static bool OptimizeLoopDevices(const std::vector<std::string>& device_list) {
    for (const auto& device : device_list) {
        unique_fd fd(open(device.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
        if (fd < 0) {
            PLOG(ERROR) << "Open failed: " << device;
            return false;
        }
        if (!LoopControl::EnableDirectIo(fd)) {
            return false;
        }
    }
    return true;
}

// Helper to use one or more loop devices around image files.
bool ImageManager::MapWithLoopDevice(const std::string& name,
                                     const std::chrono::milliseconds& timeout_ms,
                                     std::string* path) {
    auto image_header = GetImageHeaderPath(name);

    std::vector<std::string> file_list;
    if (!SplitFiemap::GetSplitFileList(image_header, &file_list)) {
        LOG(ERROR) << "Could not get image file list";
        return false;
    }

    // Map each image file as a loopback device.
    LoopControl control;
    std::vector<std::string> loop_devices;
    AutoDetachLoopDevices auto_detach(control, loop_devices);

    auto start_time = std::chrono::steady_clock::now();
    for (const auto& file : file_list) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);

        std::string loop_device;
        if (!CreateLoopDevice(control, file, timeout_ms - elapsed, &loop_device)) {
            break;
        }
        loop_devices.emplace_back(loop_device);
    }
    if (loop_devices.size() != file_list.size()) {
        // The number of devices will mismatch if CreateLoopDevice() failed.
        return false;
    }

    // If OptimizeLoopDevices fails, we'd use double the memory.
    if (!OptimizeLoopDevices(loop_devices)) {
        return false;
    }

    // If there's only one loop device (by far the most common case, splits
    // will normally only happen on sdcards with FAT32), then just return that
    // as the block device. Otherwise, we need to use dm-linear to stitch
    // together all the loop devices we just created.
    if (loop_devices.size() > 1) {
        if (!MapWithLoopDeviceList(loop_devices, name, timeout_ms, path)) {
            return false;
        }
    }

    auto status_message = "loop:" + loop_devices.back();
    auto status_file = GetStatusFilePath(name);
    if (!android::base::WriteStringToFile(status_message, status_file)) {
        PLOG(ERROR) << "Write failed: " << status_file;
        return false;
    }

    auto_detach.Commit();

    *path = loop_devices.back();
    return true;
}

bool ImageManager::MapImageDevice(const std::string& name,
                                  const std::chrono::milliseconds& timeout_ms, std::string* path) {
    if (IsImageMapped(name)) {
        LOG(ERROR) << "Backing image " << name << " is already mapped";
        return false;
    }

    auto image_header = GetImageHeaderPath(name);

    // If there is a device-mapper node wrapping the block device, then we're
    // able to create another node around it; the dm layer does not carry the
    // exclusion lock down the stack when a mount occurs.
    //
    // If there is no intermediate device-mapper node, then partitions cannot be
    // opened writable due to sepolicy and exclusivity of having a mounted
    // filesystem. This should only happen on devices with no encryption, or
    // devices with FBE and no metadata encryption. For these cases it suffices
    // to perform normal file writes to /data/gsi (which is unencrypted).
    std::string block_device;
    bool can_use_devicemapper;
    if (!FiemapWriter::GetBlockDeviceForFile(image_header, &block_device, &can_use_devicemapper)) {
        LOG(ERROR) << "Could not determine block device for " << image_header;
        return false;
    }

    if (can_use_devicemapper) {
        if (!MapWithDmLinear(*partition_opener_.get(), name, timeout_ms, path)) {
            return false;
        }
    } else if (!MapWithLoopDevice(name, timeout_ms, path)) {
        return false;
    }

    // Set a property so we remember this is mapped.
    auto prop_name = GetStatusPropertyName(name);
    if (!android::base::SetProperty(prop_name, *path)) {
        UnmapImageDevice(name, true);
        return false;
    }
    return true;
}

bool ImageManager::MapImageWithDeviceMapper(const IPartitionOpener& opener, const std::string& name,
                                            std::string* dev) {
    std::string ignore_path;
    if (!MapWithDmLinear(opener, name, {}, &ignore_path)) {
        return false;
    }

    auto& dm = DeviceMapper::Instance();
    if (!dm.GetDeviceString(name, dev)) {
        return false;
    }
    return true;
}

bool ImageManager::UnmapImageDevice(const std::string& name) {
    return UnmapImageDevice(name, false);
}

bool ImageManager::UnmapImageDevice(const std::string& name, bool force) {
    if (!force && !IsImageMapped(name)) {
        LOG(ERROR) << "Backing image " << name << " is not mapped";
        return false;
    }
    auto& dm = DeviceMapper::Instance();
    LoopControl loop;

    std::string status;
    auto status_file = GetStatusFilePath(name);
    if (!android::base::ReadFileToString(status_file, &status)) {
        PLOG(ERROR) << "Read failed: " << status_file;
        return false;
    }

    auto lines = android::base::Split(status, "\n");
    for (const auto& line : lines) {
        auto pieces = android::base::Split(line, ":");
        if (pieces.size() != 2) {
            LOG(ERROR) << "Unknown status line";
            continue;
        }
        if (pieces[0] == "dm") {
            // Failure to remove a dm node is fatal, since we can't safely
            // remove the file or loop devices.
            const auto& name = pieces[1];
            if (!dm.DeleteDeviceIfExists(name)) {
                return false;
            }
        } else if (pieces[0] == "loop") {
            // Failure to remove a loop device is not fatal, since we can still
            // remove the backing file if we want.
            loop.Detach(pieces[1]);
        } else {
            LOG(ERROR) << "Unknown status: " << pieces[0];
        }
    }

    std::string message;
    if (!android::base::RemoveFileIfExists(status_file, &message)) {
        LOG(ERROR) << "Could not remove " << status_file << ": " << message;
    }

    auto status_prop = GetStatusPropertyName(name);
    android::base::SetProperty(status_prop, "");
    return true;
}

bool ImageManager::RemoveAllImages() {
    if (!MetadataExists(metadata_dir_)) {
        return true;
    }
    auto metadata = OpenMetadata(metadata_dir_);
    if (!metadata) {
        return RemoveAllMetadata(metadata_dir_);
    }

    bool ok = true;
    for (const auto& partition : metadata->partitions) {
        auto partition_name = GetPartitionName(partition);
        ok &= DeleteBackingImage(partition_name);
    }
    return ok && RemoveAllMetadata(metadata_dir_);
}

bool ImageManager::Validate() {
    auto metadata = OpenMetadata(metadata_dir_);
    if (!metadata) {
        return false;
    }

    for (const auto& partition : metadata->partitions) {
        auto name = GetPartitionName(partition);
        auto image_path = GetImageHeaderPath(name);
        auto fiemap = SplitFiemap::Open(image_path);
        if (!fiemap || !fiemap->HasPinnedExtents()) {
            LOG(ERROR) << "Image is missing or was moved: " << image_path;
            return false;
        }
    }
    return true;
}

std::unique_ptr<MappedDevice> MappedDevice::Open(IImageManager* manager,
                                                 const std::chrono::milliseconds& timeout_ms,
                                                 const std::string& name) {
    std::string path;
    if (!manager->MapImageDevice(name, timeout_ms, &path)) {
        return nullptr;
    }

    auto device = std::unique_ptr<MappedDevice>(new MappedDevice(manager, name, path));
    if (device->fd() < 0) {
        return nullptr;
    }
    return device;
}

MappedDevice::MappedDevice(IImageManager* manager, const std::string& name, const std::string& path)
    : manager_(manager), name_(name), path_(path) {
    // The device is already mapped; try and open it.
    fd_.reset(open(path.c_str(), O_RDWR | O_CLOEXEC));
}

MappedDevice::~MappedDevice() {
    fd_ = {};
    manager_->UnmapImageDevice(name_);
}

}  // namespace fiemap
}  // namespace android
