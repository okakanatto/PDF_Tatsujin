#include "redaction_ui_test_support.h"
#include <QtTest/QTest>

namespace tatsu::testing
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
} // namespace
PageRegionPreview* ready(RedactionDialog& dialog, int page)
{
    auto preview = dynamic_cast<PageRegionPreview*>(dialog.findChild<QWidget*>("redactionPreview"));
    dialog.findChild<QComboBox*>("redactionPage")->setCurrentIndex(page);
    check(QTest::qWaitFor(
              [&] {
                  return !preview->image.isNull() && preview->property("shownPage").toInt() == page;
              },
              30000),
          "Actual asynchronous page preview");
    return preview;
}
void add(RedactionDialog& dialog, QRectF physical, int page)
{
    constexpr double mm = 72.0 / 25.4;
    auto preview = ready(dialog, page);
    auto list = dialog.findChild<QListWidget*>("redactionRegions");
    const int count = list->count();
    dialog.findChild<QPushButton*>("redactionDraw")->click();
    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier,
                      preview->physicalToWidget(physical.topLeft()).toPoint());
    QTest::mouseMove(preview, preview->physicalToWidget(physical.bottomRight()).toPoint());
    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier,
                        preview->physicalToWidget(physical.bottomRight()).toPoint());
    check(list->count() == count + 1, "Mouse drawing adds one range");
    for (const auto& value :
         QVector<QPair<QString, double>>{{"redactionX", physical.x() / mm},
                                         {"redactionY", physical.y() / mm},
                                         {"redactionWidth", physical.width() / mm},
                                         {"redactionHeight", physical.height() / mm}})
        dialog.findChild<QDoubleSpinBox*>(value.first)->setValue(value.second);
}
} // namespace tatsu::testing
