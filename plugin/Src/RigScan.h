#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Looks for a skeleton's parent table near an animator: an array of p_Bones indices, the first -1
// and each one below its own position. Reads memory through ReadProcessMemory, so bad pointers on
// the way are harmless.
namespace RigScan {
    struct Found {
        std::string path;            // how it was reached from the animator, like "+0x40 > +0x18"
        std::vector<int32_t> parents; // p_Bones entries
        int width = 0;               // 2 or 4 bytes per entry
    };

    std::vector<Found> FindParents(const void* p_Animator, size_t p_Bones, int p_Depth = 3);

    // p_Size bytes at p_At as 8-byte words ("+0x10 0000000142a3b0c8"), readable pages only.
    std::string Words(const void* p_At, size_t p_Size);
}
