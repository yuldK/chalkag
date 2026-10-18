#include "capture/frame_glow.h"

#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"

#include <algorithm>

namespace chalkag {
    void draw_frame_glow(
        SkCanvas& canvas,
        const int width,
        const int height,
        const int halo,
        const float scale,
        const std::uint32_t color
    )
    {
        canvas.clear(SK_ColorTRANSPARENT);
        canvas.save();

        const SkRect hole { SkRect::MakeXYWH(
            static_cast<float>(halo), static_cast<float>(halo), static_cast<float>(width), static_cast<float>(height)
        )
        };

        // 안쪽으로 번진 블러 픽셀까지 완전히 잘라 낸다.
        canvas.clipRect(hole, SkClipOp::kDifference, false);

        SkPaint paint {};
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);

        const float spread { static_cast<float>(halo) / 3.0f };
        paint.setStrokeWidth(std::max(2.0f * scale, spread));
        paint.setColor(SkColorSetA(color, 220));
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, std::max(0.5f, spread)));
        canvas.drawRect(hole, paint);
        paint.setStrokeWidth(scale);
        paint.setColor(SkColorSetA(color, 210));
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 1.1f * scale));
        canvas.drawRect(hole, paint);
        canvas.restore();
    }

} // namespace chalkag
