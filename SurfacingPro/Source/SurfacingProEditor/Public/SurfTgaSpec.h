// Copyright MapModelers. All Rights Reserved.
//
// The conform TGA container, byte for byte.
//
// These constants are MapLink's, taken from the bytes of its locked exemplars
// (Substance-exported TGAs and UE-written ones agree on every one of them), and
// they are kept identical here on purpose: a texture SurfacingPro conforms and
// a texture MapLink conforms are the same file, so the two libraries can be
// merged file for file one day (D55/M7). Do not "improve" a byte.

#pragma once

#include "CoreMinimal.h"

namespace SurfTgaSpec
{
    // ── Header, 18 bytes, little-endian ───────────────────────────────────
    //
    // Verified identical across the two Substance exemplars AND the four
    // UE-written fixtures in ue/Content/Materials/. One writer satisfies both,
    // which is what makes the idempotence acceptance test achievable at all.

    static constexpr uint8 IdLength        = 0;    // offset 0  — no image ID field
    static constexpr uint8 ColourMapType   = 0;    // offset 1  — no colour map
    static constexpr uint8 ImageTypeRGB    = 2;    // offset 2  — uncompressed true-colour
    static constexpr uint8 ImageTypeGray   = 3;    // offset 2  — uncompressed grayscale
    // offsets 3..7 — colour-map spec, all zero
    // offsets 8..11 — x/y origin, both zero
    static constexpr uint8 PixelDepthRGB   = 24;   // offset 16
    static constexpr uint8 PixelDepthGray  = 8;    // offset 16

    /** Offset 17. Low nibble is attribute-bits-per-pixel, high nibble bits 4-5
     *  are the origin corner.
     *
     *  Substance Designer AND Unreal both write 0x08 on 8-bit grayscale — an
     *  attribute-bits value of 8 on a single-channel image is not what the TGA
     *  2.0 spec means by that field, but it is what both producers emit and
     *  what UE's importer reads back without complaint. Emitting the spec-pure
     *  0x00 instead would fail `fc /b` on every grayscale texture. Copy the
     *  producers, not the spec. */
    static constexpr uint8 DescriptorRGB   = 0x00; // alpha bits 0, origin bottom-left
    static constexpr uint8 DescriptorGray  = 0x08; // alpha bits 8, origin bottom-left

    /** Origin bits are 00 = BOTTOM-left, so the first row in the file is the
     *  BOTTOM row of the image. FTextureSource mip 0 is top-down, so rows are
     *  written in reverse. Getting this wrong flips every texture vertically
     *  and nothing warns you. */
    static constexpr bool  bBottomUpRows   = true;

    /** 24-bit TGA channel order is B, G, R. FTextureSource BGRA8 minus its
     *  alpha byte is already in that order — no swizzle, by luck rather than
     *  design, but verified against the exemplar: file bytes 47 94 FF decode
     *  as RGB(255,148,71). */
    static constexpr bool  bBGRChannelOrder = true;

    static constexpr bool  bRunLengthEncoded = false;

    static constexpr int32 HeaderBytes = 18;
    static constexpr int32 FooterBytes = 26;

    // ── Footer, 26 bytes, TGA 2.0 ─────────────────────────────────────────
    //
    //   uint32  extension area offset   = 0  (no extension area)
    //   uint32  developer dir offset    = 0  (no developer directory)
    //   char[16] "TRUEVISION-XFILE"
    //   char     '.'   (0x2E)
    //   char     '\0'  (0x00)

    static constexpr uint32 ExtensionAreaOffset  = 0;
    static constexpr uint32 DeveloperDirOffset   = 0;
    static const     ANSICHAR* const Signature   = "TRUEVISION-XFILE";
    static constexpr uint8  SignatureDot         = 0x2E;
    static constexpr uint8  SignatureTerminator  = 0x00;

    /** Exact file size for a conform TGA. The exemplars account exactly:
     *  no padding, no ID field, no colour map, no extension area. */
    inline int64 ExpectedFileSize(int32 Width, int32 Height, bool bGrayscale)
    {
        const int64 BytesPerPixel = bGrayscale ? 1 : 3;
        return HeaderBytes
             + static_cast<int64>(Width) * Height * BytesPerPixel
             + FooterBytes;
    }
}
