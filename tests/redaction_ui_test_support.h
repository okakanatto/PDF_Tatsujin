#pragma once
#include "redaction_dialog.h"

namespace tatsu::testing
{
PageRegionPreview* ready(RedactionDialog& dialog, int page);
void add(RedactionDialog& dialog, QRectF physical, int page = 0);
} // namespace tatsu::testing
