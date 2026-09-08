#include "PipelineStateNormalize.h"

#include <string>

#include "Core/Logger.h"

namespace Kurenai::RHI
{
    BlendStateValues NormalizeBlendMode(BlendMode blendMode)
    {
        BlendStateValues values;
        switch (blendMode)
        {
        case BlendMode::AlphaBlend:
            values.Enable = true;
            values.SrcColor = BlendFactorValue::SrcAlpha;
            values.DestColor = BlendFactorValue::InvSrcAlpha;
            values.ColorOp = BlendOpValue::Add;
            values.SrcAlpha = BlendFactorValue::One;
            values.DestAlpha = BlendFactorValue::InvSrcAlpha;
            values.AlphaOp = BlendOpValue::Add;
            break;
        case BlendMode::Additive:
            values.Enable = true;
            values.SrcColor = BlendFactorValue::SrcAlpha;
            values.DestColor = BlendFactorValue::One;
            values.ColorOp = BlendOpValue::Add;
            values.SrcAlpha = BlendFactorValue::One;
            values.DestAlpha = BlendFactorValue::One;
            values.AlphaOp = BlendOpValue::Add;
            break;
        case BlendMode::Multiply:
            values.Enable = true;
            values.SrcColor = BlendFactorValue::DestColor;
            values.DestColor = BlendFactorValue::Zero;
            values.ColorOp = BlendOpValue::Add;
            values.SrcAlpha = BlendFactorValue::DestAlpha;
            values.DestAlpha = BlendFactorValue::Zero;
            values.AlphaOp = BlendOpValue::Add;
            break;
        case BlendMode::PremultipliedAlpha:
            values.Enable = true;
            values.SrcColor = BlendFactorValue::One;
            values.DestColor = BlendFactorValue::InvSrcAlpha;
            values.ColorOp = BlendOpValue::Add;
            values.SrcAlpha = BlendFactorValue::One;
            values.DestAlpha = BlendFactorValue::InvSrcAlpha;
            values.AlphaOp = BlendOpValue::Add;
            break;
        case BlendMode::Opaque:
        default:
            values.Enable = false;
            break;
        }
        return values;
    }

    DepthCompareValue NormalizeDepthCompare(bool reverseZ, bool depthAllowEqual)
    {
        if (reverseZ)
        {
            return depthAllowEqual ? DepthCompareValue::GreaterEqual : DepthCompareValue::Greater;
        }
        return depthAllowEqual ? DepthCompareValue::LessEqual : DepthCompareValue::Less;
    }

    SamplerFilterValue NormalizeSamplerFilter(SamplerFilter filter, const char* backendTag)
    {
        switch (filter)
        {
        case SamplerFilter::Anisotropic:
            return SamplerFilterValue::Anisotropic;
        case SamplerFilter::Point:
            return SamplerFilterValue::Point;
        case SamplerFilter::Linear:
            return SamplerFilterValue::Linear;
        default:
            Core::Logger::Warning(
                backendTag,
                "CreateSamplerSet: 未知のSamplerFilter(" + std::to_string(static_cast<int>(filter)) +
                    ")が指定されたためLinearで代用します");
            return SamplerFilterValue::Linear;
        }
    }

    SamplerAddressValue NormalizeSamplerAddressMode(SamplerAddressMode addressMode, const char* backendTag)
    {
        switch (addressMode)
        {
        case SamplerAddressMode::Clamp:
            return SamplerAddressValue::Clamp;
        case SamplerAddressMode::Wrap:
            return SamplerAddressValue::Wrap;
        default:
            Core::Logger::Warning(
                backendTag,
                "CreateSamplerSet: 未知のSamplerAddressMode(" + std::to_string(static_cast<int>(addressMode)) +
                    ")が指定されたためWrapで代用します");
            return SamplerAddressValue::Wrap;
        }
    }
}
