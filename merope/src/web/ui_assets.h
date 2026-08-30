// web/ui_assets.h - the single page application, embedded in the binary.
//
// The engine takes no third party dependencies and ships as one executable, so
// the UI travels with it rather than as a folder of files that can go missing.
#pragma once

#include <string_view>

namespace merope {

std::string_view ui_document();

} // namespace merope
