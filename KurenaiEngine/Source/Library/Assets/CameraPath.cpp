#include "CameraPath.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace DirectX;

namespace Kurenai::Assets
{
    namespace
    {
        bool IsFinite3(const float v[3])
        {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        }

        // キーの成分を番号で引く。0..2=位置、3=Yaw、4=Pitch。
        // 5成分すべてを同じ補間式に通すためにある(位置だけ別式にすると、
        // 位置と向きで速度の連続性が食い違う)
        float Component(const CameraPathKey& key, int index)
        {
            switch (index)
            {
            case 0: return key.Position[0];
            case 1: return key.Position[1];
            case 2: return key.Position[2];
            case 3: return key.YawRadians;
            default: return key.PitchRadians;
            }
        }

        // 3次エルミート基底。s は区間内の 0..1
        float Hermite(float p1, float m1, float p2, float m2, float s)
        {
            const float s2 = s * s;
            const float s3 = s2 * s;
            return (2.0f * s3 - 3.0f * s2 + 1.0f) * p1
                 + (s3 - 2.0f * s2 + s) * m1
                 + (-2.0f * s3 + 3.0f * s2) * p2
                 + (s3 - s2) * m2;
        }

        // 並べ替えて中央値を返す(引数は破壊する)
        float Median(std::vector<float>& values)
        {
            if (values.empty()) return 0.0f;
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        }

        XMVECTOR ForwardFromYawPitch(float yaw, float pitch)
        {
            // Core::Camera::GetForward と同じ式であること。
            // ここがずれると「検算した見かけ速度」と実際の画面の動きが食い違う
            return XMVectorSet(
                std::cos(pitch) * std::sin(yaw),
                std::sin(pitch),
                std::cos(pitch) * std::cos(yaw),
                0.0f);
        }

        // 「動いていない」とみなす位置の変化量[m/frame]。
        // 60Hzで 1e-3 m/frame = 0.06 m/s で、これは意図した移動ではありえない
        constexpr float kStillMetersPerFrame = 1.0e-3f;
        // 同じく画面上の見かけ速度[px/frame]。デノイザの再投影が意味を持つ下限として
        // 物差し側(移動中の指標のマスク)と同じ 0.5 を使う
        constexpr float kStillPixelsPerFrame = 0.5f;
    }

    bool CameraPath::SetKeys(
        std::wstring name, CameraPathInterp interp, CameraPathEnd end,
        std::vector<CameraPathKey> keys, std::string& error)
    {
        error.clear();

        if (keys.size() < 2)
        {
            error = "キーが" + std::to_string(keys.size()) + "個しかありません(2個以上必要)";
            return false;
        }

        for (size_t i = 0; i < keys.size(); ++i)
        {
            const CameraPathKey& key = keys[i];
            if (!IsFinite3(key.Position) || !std::isfinite(key.YawRadians) || !std::isfinite(key.PitchRadians))
            {
                error = "キー" + std::to_string(i) + "(Frame " + std::to_string(key.Frame) + ")に有限でない値が入っています";
                return false;
            }
            if (i > 0 && key.Frame <= keys[i - 1].Frame)
            {
                error = "Frameは狭義単調増加でなければなりません(キー" + std::to_string(i - 1)
                      + "のFrame " + std::to_string(keys[i - 1].Frame)
                      + " に対してキー" + std::to_string(i) + "が " + std::to_string(key.Frame) + ")";
                return false;
            }
        }

        // 【Yawの連続化(unwrap)】キー間の差が±180°を超えると、補間が「遠回り」する。
        // 例えば 170° → -170° は実際には 20° の回転だが、そのまま補間すると逆向きに340°回る。
        // 読み込み時に一度だけ直しておけば、EvaluatePose は素朴な補間のままでよい
        for (size_t i = 1; i < keys.size(); ++i)
        {
            float delta = keys[i].YawRadians - keys[i - 1].YawRadians;
            // 有限性は上で確認済みなので、このループは必ず停止する
            while (delta > XM_PI)
            {
                keys[i].YawRadians -= XM_2PI;
                delta = keys[i].YawRadians - keys[i - 1].YawRadians;
            }
            while (delta < -XM_PI)
            {
                keys[i].YawRadians += XM_2PI;
                delta = keys[i].YawRadians - keys[i - 1].YawRadians;
            }
        }

        m_Name = std::move(name);
        m_Interp = interp;
        m_End = end;
        m_Keys = std::move(keys);
        return true;
    }

    uint32_t CameraPath::FrameCount() const
    {
        if (!IsValid()) return 0u;
        return m_Keys.back().Frame + 1u;
    }

    CameraPathPose CameraPath::EvaluatePose(uint32_t pathFrame) const
    {
        CameraPathPose pose;
        if (!IsValid()) return pose;

        uint32_t frame = pathFrame;
        if (m_End == CameraPathEnd::Loop)
        {
            const uint32_t total = FrameCount();
            if (total > 0u) frame = pathFrame % total;
        }

        // 先頭キーより前・最終キーより後は端の姿勢で止める。
        // 【経路の開始フレームより前もここに落ちる】呼び出し側は整定待ちの間 0 を渡すので、
        // ウォームアップ中は先頭キーの姿勢で静止することになる
        if (frame <= m_Keys.front().Frame)
        {
            const CameraPathKey& key = m_Keys.front();
            pose.Position = XMFLOAT3(key.Position[0], key.Position[1], key.Position[2]);
            pose.YawRadians = key.YawRadians;
            pose.PitchRadians = key.PitchRadians;
            return pose;
        }
        if (frame >= m_Keys.back().Frame)
        {
            const CameraPathKey& key = m_Keys.back();
            pose.Position = XMFLOAT3(key.Position[0], key.Position[1], key.Position[2]);
            pose.YawRadians = key.YawRadians;
            pose.PitchRadians = key.PitchRadians;
            return pose;
        }

        // frame を含む区間 [i, i+1) を探す。Frame は狭義単調増加なので二分探索でよい
        size_t segment = 0;
        {
            size_t low = 0;
            size_t high = m_Keys.size() - 1;
            while (low + 1 < high)
            {
                const size_t mid = (low + high) / 2;
                if (m_Keys[mid].Frame <= frame) low = mid;
                else high = mid;
            }
            segment = low;
        }

        const CameraPathKey& k1 = m_Keys[segment];
        const CameraPathKey& k2 = m_Keys[segment + 1];
        const float t1 = static_cast<float>(k1.Frame);
        const float t2 = static_cast<float>(k2.Frame);
        const float dt = t2 - t1;
        const float s = (static_cast<float>(frame) - t1) / dt;

        float out[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

        if (m_Interp == CameraPathInterp::Linear)
        {
            for (int c = 0; c < 5; ++c)
            {
                const float c1 = Component(k1, c);
                const float c2 = Component(k2, c);
                out[c] = c1 + (c2 - c1) * s;
            }
        }
        else
        {
            // 非等間隔キーに対応した Catmull-Rom(有限差分の接線を区間幅で正規化したエルミート)。
            // 端では隣を自分自身にクランプするので、片側差分に自然に退化する
            // (segment==0 なら t0==t1 となり m1 = c2 - c1、つまり線形の接線になる)
            const CameraPathKey& k0 = (segment > 0) ? m_Keys[segment - 1] : k1;
            const CameraPathKey& k3 = (segment + 2 < m_Keys.size()) ? m_Keys[segment + 2] : k2;
            const float t0 = static_cast<float>(k0.Frame);
            const float t3 = static_cast<float>(k3.Frame);

            for (int c = 0; c < 5; ++c)
            {
                const float c0 = Component(k0, c);
                const float c1 = Component(k1, c);
                const float c2 = Component(k2, c);
                const float c3 = Component(k3, c);
                const float span1 = t2 - t0;
                const float span2 = t3 - t1;
                const float m1 = (span1 > 0.0f) ? ((c2 - c0) / span1) * dt : 0.0f;
                const float m2 = (span2 > 0.0f) ? ((c3 - c1) / span2) * dt : 0.0f;
                out[c] = Hermite(c1, m1, c2, m2, s);
            }
        }

        pose.Position = XMFLOAT3(out[0], out[1], out[2]);
        pose.YawRadians = out[3];
        pose.PitchRadians = out[4];
        return pose;
    }

    bool CameraPath::ComputeMotionStats(
        float fovYRadians, uint32_t renderHeight, CameraPathMotionStats& outStats) const
    {
        outStats = CameraPathMotionStats{};
        if (!IsValid()) return false;
        if (!(fovYRadians > 0.0f) || !std::isfinite(fovYRadians) || renderHeight == 0u) return false;

        const uint32_t total = FrameCount();
        if (total < 2u) return false;

        // 垂直視野角を画面の高さで割ると「1ラジアンあたり何画素か」になる。
        // 画面中心の近傍でしか正しくないが、経路が動いているかの検算にはこれで足りる
        const float pixelsPerRadian = static_cast<float>(renderHeight) / fovYRadians;

        std::vector<float> meters;
        std::vector<float> degrees;
        std::vector<float> pixels;
        meters.reserve(total - 1u);
        degrees.reserve(total - 1u);
        pixels.reserve(total - 1u);

        CameraPathPose previous = EvaluatePose(0u);
        XMVECTOR previousForward = ForwardFromYawPitch(previous.YawRadians, previous.PitchRadians);

        uint32_t stillFrames = 0u;
        for (uint32_t frame = 1u; frame < total; ++frame)
        {
            const CameraPathPose current = EvaluatePose(frame);
            const XMVECTOR currentForward = ForwardFromYawPitch(current.YawRadians, current.PitchRadians);

            const XMVECTOR delta = XMVectorSubtract(XMLoadFloat3(&current.Position), XMLoadFloat3(&previous.Position));
            const float metersPerFrame = XMVectorGetX(XMVector3Length(delta));

            float cosAngle = XMVectorGetX(XMVector3Dot(previousForward, currentForward));
            cosAngle = std::min(1.0f, std::max(-1.0f, cosAngle));
            const float radiansPerFrame = std::acos(cosAngle);
            const float pixelsPerFrame = radiansPerFrame * pixelsPerRadian;

            meters.push_back(metersPerFrame);
            degrees.push_back(XMConvertToDegrees(radiansPerFrame));
            pixels.push_back(pixelsPerFrame);

            // 【両方が止まっていて初めて「静止」】回転していなくても前進していれば画面は動く。
            // 逆に位置が同じでも回れば動く。片方だけで判定すると、動いている経路を
            // 静止と誤判定して拒否してしまう
            if (pixelsPerFrame < kStillPixelsPerFrame && metersPerFrame < kStillMetersPerFrame)
            {
                ++stillFrames;
            }

            previous = current;
            previousForward = currentForward;
        }

        outStats.FrameCount = total;
        outStats.StillFrameCount = stillFrames;
        outStats.MinMetersPerFrame = *std::min_element(meters.begin(), meters.end());
        outStats.MaxMetersPerFrame = *std::max_element(meters.begin(), meters.end());
        outStats.MinDegreesPerFrame = *std::min_element(degrees.begin(), degrees.end());
        outStats.MaxDegreesPerFrame = *std::max_element(degrees.begin(), degrees.end());
        outStats.MinPixelsPerFrame = *std::min_element(pixels.begin(), pixels.end());
        outStats.MaxPixelsPerFrame = *std::max_element(pixels.begin(), pixels.end());
        // Median は引数を並べ替えるので、min/max を採り終えてから呼ぶこと
        outStats.MedianMetersPerFrame = Median(meters);
        outStats.MedianDegreesPerFrame = Median(degrees);
        outStats.MedianPixelsPerFrame = Median(pixels);
        return true;
    }
}
