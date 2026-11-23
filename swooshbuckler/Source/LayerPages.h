#pragma once

#include "DetailLayer.h"

#include <memory>

/**
 * THE LAYER PAGES: what is behind a notched control, one page for each Layers::Kind, laid out on DetailLayer's grid
 * against the parameter IDs in Parameters.h.
 *
 *   Swirl     choices: Type; then by type
 *               Phaser + Flanger, Phaser, Flanger: Feedback, Speed, Depth, Follow
 *               Tape Flanger: choices Motion and the Invert key; Depth, Rate, Feedback, Wobble, Varispeed (springs
 *                 to the middle on release; the OffsetMeter under it; a wire target)
 *               Doubler: choices Wave, keys Thicken and Spread side by side; Delay, Wander, Rate, Feedback
 *   Filter    choices: Type, the 24 dB key, Shape; Closed At, Resonance, Follow, Sweep, Rate
 *   Together  box: Hit Every; Rate (the front wheel's own parameter, live in every Ride Mode), Peak At, Stagger
 *   Swell     box: Auto Swell; Every, Hold, Chance, Loose
 *
 * A box whose setting does not say what it is ("Sine", "Random", "1 Peak", "Off", "With the Pass") has its name in
 * small type over it: Motion, Wave, Shape, Hit Every, Auto Swell. Ride's pages keep to the wheels' part of the row
 * and take clicks only on their controls: the panel's keys (Auto Ride, the Ride Mode box, Throw) stay under them.
 *
 * Each control is bound to its parameter with an ordinary attachment (so it is undone like any other move, but for
 * Varispeed, which the processor keeps out of the history), reads in its own units, and has a plain tooltip: a
 * heading (the layer's title, then the control's name: "Bus A / Swirl: Motion"), then Params::tipFor(id). A control
 * that does nothing in the current choice takes the Resting look and an
 * "It rests..." line, which refresh() keeps up to date: Rate and Shape while Sweep is 0%; Rate but
 * On Its Own (Tape Flanger); Feedback while Thicken is on (Doubler); the Together page outside Faders Together and
 * Hit Every with Aim off; the Swell page while Auto Ride is off or Energy is 0, everything on it while Auto Swell is
 * Off, Every and Loose but On a Clock, and On Peaks in Faders Together.
 *
 * What the host (DetailLayer) relies on: make() returns a page for every target, never null; the page lays itself
 * out in resized() inside the bounds it is given (DetailLayer::pageWidth(target) x DetailLayer::c_rowHeight) and
 * keeps its attachments alive for as long as it exists; refresh() about 30 times a second while it is open; on a
 * Swirl page wireTargets() is the Varispeed wheel (hidden, and so out of reach of a wire, unless the type is Tape
 * Flanger).
 */
namespace LayerPages
{
    std::unique_ptr<LayerPage> make(const LayerPage::Context& context);
}
