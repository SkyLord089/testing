// Native Windows file dialog for picking a .gguf model.
// Kept in its own translation unit so the HTTP layer stays platform-neutral and
// the Windows code can be syntax-checked / unit-tested in isolation.
#pragma once

#include <string>

namespace dualmoe {

// Shows the standard Windows "Open file" dialog filtered to *.gguf.
// Returns true and fills `out_path` when the user picks a file.
// Returns false when the user cancels or when the platform has no native dialog.
bool pick_gguf_file(const std::string & start_dir, std::string & out_path, bool & canceled);

// False on non-Windows builds (the UI then falls back to the in-app browser).
bool native_file_dialog_available();

} // namespace dualmoe
