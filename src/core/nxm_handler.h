#pragma once

#include <string>

namespace gtkdrop::nxm_handler {

// Whether GTKdrop is the system's handler for nxm:// links (Nexus' "Mod Manager Download" buttons).
bool is_default();

// Writes ~/.local/share/applications/dev.gtkdrop.GTKdrop.desktop (running this executable with the link) and
// makes it the default nxm:// handler. Rebuilding into another folder means registering again.
bool make_default(std::string *error);

// Drops the nxm:// association again (the system falls back to any other handler, e.g. another mod manager).
void unset_default();

} // namespace gtkdrop::nxm_handler
