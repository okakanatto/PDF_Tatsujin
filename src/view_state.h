#pragma once
#include <QLineF>
#include <QPointF>
#include <QtGlobal>

namespace tatsu
{
struct ViewAnchor
{
    int page = -1;
    QPointF point;
    QPointF ratio{.5, .5};
};
struct ViewState
{
    ViewAnchor anchor;
    double zoom = 1;
    int fitMode = 0, fitReference = 0;
    quint64 activeSearch = 0;

    // Ignore subpixel layout noise, but preserve how a fitted view is restored.
    bool samePosition(const ViewState& other) const
    {
        return anchor.page == other.anchor.page && anchor.ratio == other.anchor.ratio &&
               QLineF(anchor.point, other.anchor.point).length() <= .5 &&
               qAbs(zoom - other.zoom) <= .001 && fitMode == other.fitMode &&
               (!fitMode || fitReference == other.fitReference);
    }
};
} // namespace tatsu
