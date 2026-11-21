#pragma once

#include "DetailLayer.h"

#include <memory>

/**
 * THE LAYER PAGES (a stub: every page is empty; filled in by the pages' work package).
 *
 * One page for each Layers::Kind, laid out on DetailLayer's grid, against the parameter IDs in Parameters.h:
 *   Swirl     choices: Type (ParamIDs::swirlTypeNames); then by type
 *               Phaser + Flanger, Phaser, Flanger: Feedback, Speed, Depth, Follow
 *               Tape Flanger: choices Motion and the Invert key; Depth, Rate, Feedback, Wobble, Varispeed (springs
 *                 to the middle on release; the OffsetMeter under it; a wire target)
 *               Doubler: choices Wave, keys Thicken and Spread side by side; Delay, Wander, Rate, Feedback
 *   Filter    choices: Type, the 24 dB key, Shape; Closed At, Resonance, Follow, Sweep, Rate
 *   Together  box: Hit Every; Peak At, Stagger
 *   Swell     box: Auto Swell; Every, Hold, Chance, Loose
 * Captions are the parameter's name after "Bus A " and the group word ("Feedback", "Closed At"). Tooltips:
 * Params::tipFor(id). A control that does nothing in the current choice takes the Resting look and an "It rests..."
 * line (Rate while Sweep is 0%, Feedback while Thicken is on, and the others the design lists). Every text inside its
 * box at 0.85, 1.0 and 1.5 window scale.
 *
 * What the host (DetailLayer) relies on: make() returns a page for every target, never null; the page lays itself
 * out in resized() inside the bounds it is given and keeps its attachments alive for as long as it exists.
 */
namespace LayerPages
{
    std::unique_ptr<LayerPage> make(const LayerPage::Context& context);
}
