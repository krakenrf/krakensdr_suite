#pragma once

// Folders and drives for the features that write to the disk (🗂 Decoder
// Logging, 📼 Recorder): create a folder, free space, the drives a page may
// offer (/ and what is mounted under /media, /mnt, /run/media, /srv; on
// Windows the drive letters - fixed, removable with a medium, RAM disks).
// Windows paths come back '/'-separated ("C:/x", "//srv/share/x").

#include <cstdint>
#include <string>

namespace diskutil {

bool mkdirs(const std::string& path);           // like mkdir -p; true if it is a folder now
std::string absolute(const std::string& path);  // relative = inside the working directory
// free (for unprivileged users) / total bytes of the file system holding path
// (or its nearest existing parent)
bool disk_space(std::string path, uint64_t* free_b, uint64_t* total_b);
// the file system type of path's drive ("ext4", "vfat", ...; "" = unknown) -
// vfat (FAT32) can't hold a file of 4 GiB. Windows: "vfat" (FAT32 / FAT),
// "exfat", "ntfs", "refs"
std::string fs_type(const std::string& path);
// [{path, dev, type, free, total}, ...] as JSON - the folder's own drive always
// included (its mount point -> *mount_of_dir; Windows: "C:/", "//srv/share").
// Skips network / FUSE mounts (Windows: network and CD / DVD drives): statvfs
// on a dead one blocks, and this runs on the uWS loop
std::string mounts_json(const std::string& dir_abs, std::string* mount_of_dir);

}  // namespace diskutil
