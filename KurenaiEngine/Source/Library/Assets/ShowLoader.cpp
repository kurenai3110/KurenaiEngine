#include "ShowLoader.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "Core/Logger.h"
#include "Core/StringUtil.h"
#include "ShowPackage.h"

namespace Kurenai::Assets
{
    namespace
    {
        using Core::WideToUtf8;

        // StringPool(offset,length)からUTF-8部分文字列を安全に取り出す。壊れた.kshowが
        // 範囲外を指していてもプロセスを異常終了させないよう、必ず範囲チェックを行う
        // (ModelLoader.cppの同名関数と同じ扱い)
        std::string ReadPoolString(const std::string& pool, uint32_t offset, uint32_t length, const char* fieldNameForError)
        {
            if (static_cast<uint64_t>(offset) + length > pool.size())
            {
                throw std::runtime_error(std::string("ショーのStringPool参照が範囲外です: ") + fieldNameForError);
            }
            return pool.substr(offset, length);
        }
    }

    float ShowLoopDuration(const ShowData& data)
    {
        if (data.Formations.empty())
        {
            return 0.0f;
        }
        // HoldとMorphがどちらも0でも進行が止まらないよう下限を置く(0除算とゼロ長ループの回避)
        const float segment = std::max(0.01f, data.HoldSeconds + data.MorphSeconds);
        return segment * static_cast<float>(data.Formations.size());
    }

    ShowData LoadShow(const std::wstring& filePath)
    {
        std::ifstream in;
        in.open(filePath, std::ios::binary);
        if (!in.is_open())
        {
            throw std::runtime_error("ショーファイルを開けませんでした: " + WideToUtf8(filePath));
        }

        ShowHeader header{};
        std::vector<FormationEntry> entries;
        std::vector<ShowPoint> points;
        std::string stringPool;

        try
        {
            in.exceptions(std::ios::failbit | std::ios::badbit);

            in.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (std::memcmp(header.Magic, kShowMagic, sizeof(kShowMagic)) != 0)
            {
                throw std::runtime_error("マジックナンバーが不正です");
            }
            if (header.Version != kShowVersion)
            {
                throw std::runtime_error(
                    "バージョンが対応していません(ファイル: " + std::to_string(header.Version) +
                    ", ランタイム: " + std::to_string(kShowVersion) + ")");
            }
            if (header.PointStride != sizeof(ShowPoint))
            {
                throw std::runtime_error("点のレイアウトが現在のランタイムと一致しません");
            }
            if (header.DroneCount == 0u || header.FormationCount == 0u)
            {
                throw std::runtime_error("機体数または編隊数が0です");
            }

            entries.resize(header.FormationCount);
            in.read(
                reinterpret_cast<char*>(entries.data()),
                static_cast<std::streamsize>(entries.size() * sizeof(FormationEntry)));

            // 1500機×6編隊で216KB。丸ごと読んでからばらす
            points.resize(static_cast<size_t>(header.DroneCount) * header.FormationCount);
            in.read(
                reinterpret_cast<char*>(points.data()),
                static_cast<std::streamsize>(points.size() * sizeof(ShowPoint)));

            stringPool.resize(header.StringPoolSize);
            if (header.StringPoolSize > 0u)
            {
                in.read(stringPool.data(), static_cast<std::streamsize>(stringPool.size()));
            }
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("ショーファイルの読み込みに失敗しました(" + WideToUtf8(filePath) + "): " + e.what());
        }

        ShowData data{};
        data.DroneCount = header.DroneCount;
        data.Speed = header.Speed;
        data.HoldSeconds = header.HoldSeconds;
        data.MorphSeconds = header.MorphSeconds;
        data.Brightness = header.Brightness;
        data.Radius = header.Radius;
        data.HoverAmplitude = header.HoverAmplitude;
        data.Seed = header.Seed;

        data.Formations.resize(header.FormationCount);
        for (uint32_t f = 0; f < header.FormationCount; ++f)
        {
            ShowFormation& formation = data.Formations[f];
            formation.Name = ReadPoolString(stringPool, entries[f].NameOffset, entries[f].NameLength, "FormationName");
            formation.Positions.resize(header.DroneCount);
            formation.Colors.resize(header.DroneCount);

            const ShowPoint* src = points.data() + static_cast<size_t>(f) * header.DroneCount;
            for (uint32_t i = 0; i < header.DroneCount; ++i)
            {
                formation.Positions[i] = DirectX::XMFLOAT3(src[i].Position[0], src[i].Position[1], src[i].Position[2]);
                formation.Colors[i] = DirectX::XMFLOAT3(src[i].Color[0], src[i].Color[1], src[i].Color[2]);
            }
        }

        Core::Logger::Info(
            "ShowLoader",
            "ショーを読み込みました (" + WideToUtf8(filePath) + ", 機体数: " + std::to_string(data.DroneCount) +
                ", 編隊: " + std::to_string(data.Formations.size()) + ", 1巡: " +
                std::to_string(ShowLoopDuration(data)) + "秒)");

        return data;
    }

    void SaveShow(const std::wstring& filePath, const ShowData& data)
    {
        if (data.DroneCount == 0u || data.Formations.empty())
        {
            throw std::runtime_error("機体数または編隊数が0のショーは保存できません");
        }
        for (size_t f = 0; f < data.Formations.size(); ++f)
        {
            const ShowFormation& formation = data.Formations[f];
            // 【点数の食い違いはここで止める】モーフは形Aのi番目と形Bのi番目を結ぶだけなので、
            // 揃っていないファイルを書くと変形の途中で機体が消える。揃えるのはエディタの
            // 責任だが、書き出しはその結果が外へ出る最後の地点なので関所として検査する
            if (formation.Positions.size() != data.DroneCount || formation.Colors.size() != data.DroneCount)
            {
                throw std::runtime_error(
                    "編隊" + std::to_string(f) + "の点数がDroneCountと一致しません(位置: " +
                    std::to_string(formation.Positions.size()) + ", 色: " + std::to_string(formation.Colors.size()) +
                    ", DroneCount: " + std::to_string(data.DroneCount) + ")");
            }
        }

        // StringPoolを先に組む(オフセットが確定しないとFormationEntryを書けないため)
        std::string stringPool;
        std::vector<FormationEntry> entries(data.Formations.size());
        for (size_t f = 0; f < data.Formations.size(); ++f)
        {
            const std::string& name = data.Formations[f].Name;
            entries[f] = {};
            entries[f].NameOffset = static_cast<uint32_t>(stringPool.size());
            entries[f].NameLength = static_cast<uint32_t>(name.size());
            stringPool += name;
        }

        ShowHeader header{};
        std::memcpy(header.Magic, kShowMagic, sizeof(kShowMagic));
        header.Version = kShowVersion;
        header.PointStride = sizeof(ShowPoint);
        header.DroneCount = data.DroneCount;
        header.FormationCount = static_cast<uint32_t>(data.Formations.size());
        header.Speed = data.Speed;
        header.HoldSeconds = data.HoldSeconds;
        header.MorphSeconds = data.MorphSeconds;
        header.Brightness = data.Brightness;
        header.Radius = data.Radius;
        header.HoverAmplitude = data.HoverAmplitude;
        header.Seed = data.Seed;
        header.StringPoolSize = static_cast<uint32_t>(stringPool.size());

        std::vector<ShowPoint> points(static_cast<size_t>(data.DroneCount) * data.Formations.size());
        for (size_t f = 0; f < data.Formations.size(); ++f)
        {
            const ShowFormation& formation = data.Formations[f];
            ShowPoint* dst = points.data() + f * data.DroneCount;
            for (uint32_t i = 0; i < data.DroneCount; ++i)
            {
                dst[i].Position[0] = formation.Positions[i].x;
                dst[i].Position[1] = formation.Positions[i].y;
                dst[i].Position[2] = formation.Positions[i].z;
                dst[i].Color[0] = formation.Colors[i].x;
                dst[i].Color[1] = formation.Colors[i].y;
                dst[i].Color[2] = formation.Colors[i].z;
            }
        }

        std::error_code ec;
        const std::filesystem::path path(filePath);
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path(), ec);
        }

        std::ofstream out;
        out.open(filePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            throw std::runtime_error("ショーファイルを作成できませんでした: " + WideToUtf8(filePath));
        }

        try
        {
            out.exceptions(std::ios::failbit | std::ios::badbit);
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            out.write(
                reinterpret_cast<const char*>(entries.data()),
                static_cast<std::streamsize>(entries.size() * sizeof(FormationEntry)));
            out.write(
                reinterpret_cast<const char*>(points.data()),
                static_cast<std::streamsize>(points.size() * sizeof(ShowPoint)));
            if (!stringPool.empty())
            {
                out.write(stringPool.data(), static_cast<std::streamsize>(stringPool.size()));
            }
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("ショーファイルの書き込みに失敗しました(" + WideToUtf8(filePath) + "): " + e.what());
        }

        Core::Logger::Info(
            "ShowLoader",
            "ショーを保存しました (" + WideToUtf8(filePath) + ", 機体数: " + std::to_string(data.DroneCount) +
                ", 編隊: " + std::to_string(data.Formations.size()) + ")");
    }
}
