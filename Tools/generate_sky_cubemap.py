import math
import os
import struct

FACE_SIZE = 256
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Assets", "Skybox")
OUT_PATH = os.path.join(OUT_DIR, "Sky.dds")

# 地平線付近は明るい水色、天頂は濃い青空色へグラデーションする
HORIZON_COLOR = (0.80, 0.88, 0.97)
ZENITH_COLOR = (0.20, 0.45, 0.85)


def face_direction(face, u, v):
    # D3Dのキューブマップ標準の面->方向マッピング(u, vは-1..1)
    if face == 0:  # +X
        return (1.0, -v, -u)
    if face == 1:  # -X
        return (-1.0, -v, u)
    if face == 2:  # +Y
        return (u, 1.0, v)
    if face == 3:  # -Y
        return (u, -1.0, -v)
    if face == 4:  # +Z
        return (u, -v, 1.0)
    return (-u, -v, -1.0)  # -Z


def normalize(v):
    length = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (v[0] / length, v[1] / length, v[2] / length)


def sky_color(dir_y):
    # dir_yは-1(真下)..1(真上)。水平線よりやや下まで空色を伸ばしつつ、
    # 真上に近づくほど濃い青になるよう0..1の補間係数に変換する
    t = max(0.0, min(1.0, dir_y + 0.15))
    r = HORIZON_COLOR[0] + (ZENITH_COLOR[0] - HORIZON_COLOR[0]) * t
    g = HORIZON_COLOR[1] + (ZENITH_COLOR[1] - HORIZON_COLOR[1]) * t
    b = HORIZON_COLOR[2] + (ZENITH_COLOR[2] - HORIZON_COLOR[2]) * t
    return (r, g, b)


def build_face_bytes(face):
    data = bytearray()
    for y in range(FACE_SIZE):
        v = (2.0 * (y + 0.5) / FACE_SIZE) - 1.0
        for x in range(FACE_SIZE):
            u = (2.0 * (x + 0.5) / FACE_SIZE) - 1.0
            dir_vec = normalize(face_direction(face, u, v))
            r, g, b = sky_color(dir_vec[1])
            data += bytes((
                int(max(0.0, min(1.0, r)) * 255.0 + 0.5),
                int(max(0.0, min(1.0, g)) * 255.0 + 0.5),
                int(max(0.0, min(1.0, b)) * 255.0 + 0.5),
                255,
            ))
    return bytes(data)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    DDSD_CAPS = 0x1
    DDSD_HEIGHT = 0x2
    DDSD_WIDTH = 0x4
    DDSD_PITCH = 0x8
    DDSD_PIXELFORMAT = 0x1000
    header_flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT

    DDPF_FOURCC = 0x4

    DDSCAPS_TEXTURE = 0x1000
    DDSCAPS_COMPLEX = 0x8
    caps = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX

    DDSCAPS2_CUBEMAP = 0x200
    DDSCAPS2_CUBEMAP_POSITIVEX = 0x400
    DDSCAPS2_CUBEMAP_NEGATIVEX = 0x800
    DDSCAPS2_CUBEMAP_POSITIVEY = 0x1000
    DDSCAPS2_CUBEMAP_NEGATIVEY = 0x2000
    DDSCAPS2_CUBEMAP_POSITIVEZ = 0x4000
    DDSCAPS2_CUBEMAP_NEGATIVEZ = 0x8000
    caps2 = (DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEX | DDSCAPS2_CUBEMAP_NEGATIVEX |
             DDSCAPS2_CUBEMAP_POSITIVEY | DDSCAPS2_CUBEMAP_NEGATIVEY |
             DDSCAPS2_CUBEMAP_POSITIVEZ | DDSCAPS2_CUBEMAP_NEGATIVEZ)

    pitch = FACE_SIZE * 4

    header = bytearray()
    header += struct.pack("<I", 124)              # dwSize
    header += struct.pack("<I", header_flags)      # dwFlags
    header += struct.pack("<I", FACE_SIZE)         # dwHeight
    header += struct.pack("<I", FACE_SIZE)         # dwWidth
    header += struct.pack("<I", pitch)             # dwPitchOrLinearSize
    header += struct.pack("<I", 0)                 # dwDepth
    header += struct.pack("<I", 1)                 # dwMipMapCount
    header += b"\x00" * 44                          # dwReserved1[11]
    # DDS_PIXELFORMAT
    header += struct.pack("<I", 32)                # ddspf.dwSize
    header += struct.pack("<I", DDPF_FOURCC)       # ddspf.dwFlags
    header += b"DX10"                                # ddspf.dwFourCC
    header += struct.pack("<I", 0)                 # ddspf.dwRGBBitCount
    header += struct.pack("<I", 0)                 # ddspf.dwRBitMask
    header += struct.pack("<I", 0)                 # ddspf.dwGBitMask
    header += struct.pack("<I", 0)                 # ddspf.dwBBitMask
    header += struct.pack("<I", 0)                 # ddspf.dwABitMask
    header += struct.pack("<I", caps)              # dwCaps
    header += struct.pack("<I", caps2)             # dwCaps2
    header += struct.pack("<I", 0)                 # dwCaps3
    header += struct.pack("<I", 0)                 # dwCaps4
    header += struct.pack("<I", 0)                 # dwReserved2
    assert len(header) == 124, len(header)

    DXGI_FORMAT_R8G8B8A8_UNORM = 28
    D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3
    DDS_RESOURCE_MISC_TEXTURECUBE = 0x4

    header_dxt10 = bytearray()
    header_dxt10 += struct.pack("<I", DXGI_FORMAT_R8G8B8A8_UNORM)
    header_dxt10 += struct.pack("<I", D3D10_RESOURCE_DIMENSION_TEXTURE2D)
    header_dxt10 += struct.pack("<I", DDS_RESOURCE_MISC_TEXTURECUBE)
    header_dxt10 += struct.pack("<I", 1)  # arraySize (キューブ数)
    header_dxt10 += struct.pack("<I", 0)  # miscFlags2
    assert len(header_dxt10) == 20

    with open(OUT_PATH, "wb") as f:
        f.write(b"DDS ")
        f.write(header)
        f.write(header_dxt10)
        # D3Dキューブマップの面順: +X, -X, +Y, -Y, +Z, -Z
        for face in range(6):
            f.write(build_face_bytes(face))

    print(f"wrote {OUT_PATH} ({FACE_SIZE}x{FACE_SIZE} x6 faces)")


if __name__ == "__main__":
    main()
