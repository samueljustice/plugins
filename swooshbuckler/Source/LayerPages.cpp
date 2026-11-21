#include "LayerPages.h"

// A stub: an empty page for every target. See LayerPages.h for what the filler provides.

namespace LayerPages
{
    std::unique_ptr<LayerPage> make(const LayerPage::Context& context)
    {
        return std::make_unique<LayerPage>(context);
    }
}
