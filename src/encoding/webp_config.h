#pragma once

#include "capture/image_capture.h"

#include <webp/encode.h>

namespace chalkag::encoding {
    inline bool configure_webp(
        WebPConfig& config,
        const lossless_compression compression,
        const bool lossless,
        const int quality,
        const bool threading
    )
    {
        if (!WebPConfigInit(&config))
        {
            return false;
        }
        if (lossless)
        {
            const int level { compression == lossless_compression::fast          ? 0
                                  : compression == lossless_compression::maximum ? 9
                                                                                 : 6
            };
            if (!WebPConfigLosslessPreset(&config, level))
            {
                return false;
            }
        }
        else
        {
            config.quality = static_cast<float>(quality);
            config.method = compression == lossless_compression::fast ? 0 : 6;
        }
        config.exact = 1;
        // 최대 압축의 변환 탐색만 내부 병렬화한다. 프레임 병렬화와 중첩하지 않는다.
        config.thread_level = lossless && compression == lossless_compression::maximum && threading ? 1 : 0;
        return WebPValidateConfig(&config) != 0;
    }
} // namespace chalkag::encoding
