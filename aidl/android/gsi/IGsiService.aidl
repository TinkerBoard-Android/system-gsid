/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.gsi;

import android.gsi.GsiInstallParams;
import android.gsi.GsiProgress;
import android.gsi.IImageManager;
import android.os.ParcelFileDescriptor;

/** {@hide} */
interface IGsiService {
    /* Status codes for GsiProgress.status */
    const int STATUS_NO_OPERATION = 0;
    const int STATUS_WORKING = 1;
    const int STATUS_COMPLETE = 2;

    /* Install succeeded. */
    const int INSTALL_OK = 0;
    /* Install failed with a generic system error. */
    const int INSTALL_ERROR_GENERIC = 1;
    /* Install failed because there was no free space. */
    const int INSTALL_ERROR_NO_SPACE = 2;
    /**
     * Install failed because the file system was too fragmented or did not
     * have enough additional free space.
     */
    const int INSTALL_ERROR_FILE_SYSTEM_CLUTTERED = 3;

    /**
     * Write bytes from a stream to the on-disk GSI.
     *
     * @param stream        Stream descriptor.
     * @param bytes         Number of bytes that can be read from stream.
     * @return              true on success, false otherwise.
     */
    boolean commitGsiChunkFromStream(in ParcelFileDescriptor stream, long bytes);

    /**
     * Query the progress of the current asynchronous install operation. This
     * can be called while another operation is in progress.
     */
    GsiProgress getInstallProgress();

    /**
     * Write bytes from memory to the on-disk GSI.
     *
     * @param bytes         Byte array.
     * @return              true on success, false otherwise.
     */
    boolean commitGsiChunkFromMemory(in byte[] bytes);

    /**
     * Complete a GSI installation and mark it as bootable. The caller is
     * responsible for rebooting the device as soon as possible.
     *
     * @param oneShot       If true, the GSI will boot once and then disable itself.
     *                      It can still be re-enabled again later with setGsiBootable.
     * @return              INSTALL_* error code.
     */
    int enableGsi(boolean oneShot);

    /**
     * @return              True if Gsi is enabled
     */
    boolean isGsiEnabled();

    /**
     * Cancel an in-progress GSI install.
     */
    boolean cancelGsiInstall();

    /**
     * Return if a GSI installation is currently in-progress.
     */
    boolean isGsiInstallInProgress();

    /**
     * Remove a GSI install. This will completely remove and reclaim space used
     * by the GSI and its userdata. If currently running a GSI, space will be
     * reclaimed on the reboot.
     *
     * @return              true on success, false otherwise.
     */
    boolean removeGsi();

    /**
     * Disables a GSI install. The image and userdata will be retained, but can
     * be re-enabled at any time with setGsiBootable.
     */
    boolean disableGsi();

    /**
     * Returns true if a gsi is installed.
     */
    boolean isGsiInstalled();
    /**
     * Returns true if the gsi is currently running, false otherwise.
     */
    boolean isGsiRunning();

    /**
     * If a GSI is installed, returns the directory where the installed images
     * are located. Otherwise, returns an empty string.
     */
     @utf8InCpp String getInstalledGsiImageDir();

    /**
     * Begin a GSI installation.
     *
     * This is a replacement for startGsiInstall, in order to supply additional
     * options.
     *
     * @return              0 on success, an error code on failure.
     */
    int beginGsiInstall(in GsiInstallParams params);

    /**
     * Wipe the userdata of an existing GSI install. This will not work if the
     * GSI is currently running. The userdata image will not be removed, but the
     * first block will be zeroed ensuring that the next GSI boot formats /data.
     *
     * @return              0 on success, an error code on failure.
     */
    int wipeGsiUserdata();

    /**
     * Open a handle to an IImageManager for the given metadata and data storage paths.
     *
     * @param prefix        A prefix used to organize images. The data path will become
     *                      /data/gsi/{prefix} and the metadata path will become
     *                      /metadata/gsi/{prefix}.
     */
    IImageManager openImageManager(@utf8InCpp String prefix);
}
