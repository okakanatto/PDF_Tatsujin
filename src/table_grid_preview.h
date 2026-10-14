#pragma once
#include "page_region_preview.h"
#include "table_extraction.h"

namespace tatsu
{
class TableGridPreview : public PageRegionPreview
{
public:
    explicit TableGridPreview(QWidget* parent = nullptr);
    TableGrid grid;
    std::function<bool(const TableGrid&)> changed;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    int boundary = -1;
    bool column = false;
    TableGrid before;
};
} // namespace tatsu
