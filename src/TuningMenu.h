#pragma once

namespace ShoutMCO::TuningMenu {
    // Register the in-game tuning page with SKSE Menu Framework.
    //
    // No-op, with one log line, when the framework is not installed. The menu is a convenience over
    // a file the player can already edit in a text editor, so its absence is not an error and must
    // never stop the engine loading.
    void Install();
}
