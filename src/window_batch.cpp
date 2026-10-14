#include "batch_dialog.h"
#include "window.h"
namespace tatsu
{
void Window::processMultipleDocuments()
{
    if (doc.busy)
        fail("現在のOCR処理が終わってから一括処理を開始してください。");
    BatchDialog dialog(this);
    dialog.exec();
}
} // namespace tatsu
