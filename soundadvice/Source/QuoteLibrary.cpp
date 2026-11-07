#include "QuoteLibrary.h"

#include "BinaryData.h"

#include <algorithm>
#include <chrono>
#include <map>

const QuoteLibrary& QuoteLibrary::getEmbedded()
{
    static const QuoteLibrary library = []
    {
        QuoteLibrary lib;
        const auto error = lib.loadFromJson (juce::String::fromUTF8 (BinaryData::quotes_json, BinaryData::quotes_jsonSize));
        jassert (error.isEmpty());
        juce::ignoreUnused (error);
        return lib;
    }();
    return library;
}

juce::String QuoteLibrary::loadFromJson (const juce::String& jsonText)
{
    people.clear();
    entries.clear();
    categories.clear();

    juce::var root;
    const auto parsed = juce::JSON::parse (jsonText, root);
    if (parsed.failed())
        return "JSON: " + parsed.getErrorMessage();

    categories.push_back ({ allCategoryId, "Anything", {} });
    std::map<juce::String, int> categoryIndex;

    if (auto* cats = root["categories"].getArray())
    {
        for (const auto& c : *cats)
        {
            const auto id = c["id"].toString();
            if (id.isEmpty() || id == allCategoryId || categoryIndex.count (id) > 0)
                return "bad or duplicate category id: " + id;
            categoryIndex[id] = (int) categories.size();
            categories.push_back ({ id, c["name"].toString(), {} });
        }
    }

    std::map<juce::String, int> personIndex;
    if (auto* ppl = root["people"].getArray())
    {
        for (const auto& p : *ppl)
        {
            Person person { p["name"].toString(), p["role"].toString(), p["credits"].toString(), p["imdb"].toString() };
            if (person.name.isEmpty() || personIndex.count (person.name) > 0)
                return "bad or duplicate person: " + person.name;
            personIndex[person.name] = (int) people.size();
            people.push_back (std::move (person));
        }
    }

    std::map<juce::String, int> seenIds;
    if (auto* list = root["entries"].getArray())
    {
        for (const auto& e : *list)
        {
            Entry entry;
            entry.id = e["id"].toString();
            entry.text = e["text"].toString();
            entry.verbatim = e["kind"].toString() == "quote";

            const auto who = e["person"].toString();
            const auto p = personIndex.find (who);
            if (p == personIndex.end())
                return "entry " + entry.id + ": unknown person " + who;
            entry.person = p->second;

            if (entry.id.isEmpty() || seenIds.count (entry.id) > 0)
                return "bad or duplicate entry id: " + entry.id;
            if (entry.text.isEmpty())
                return "entry " + entry.id + " has no text";

            const auto& src = e["source"];
            entry.source = { src["title"].toString(), src["publication"].toString(), src["url"].toString(),
                             src["year"].isVoid() ? 0 : (int) src["year"] };

            const int index = (int) entries.size();
            if (auto* cats = e["categories"].getArray())
            {
                for (const auto& c : *cats)
                {
                    const auto id = c.toString();
                    const auto found = categoryIndex.find (id);
                    if (found == categoryIndex.end())
                        return "entry " + entry.id + ": unknown category " + id;
                    if (! entry.categories.contains (id))
                    {
                        entry.categories.add (id);
                        categories[(size_t) found->second].entries.push_back (index);
                    }
                }
            }
            if (entry.categories.isEmpty())
                return "entry " + entry.id + " has no category";

            seenIds[entry.id] = index;
            categories[0].entries.push_back (index);
            entries.push_back (std::move (entry));
        }
    }

    if (entries.empty())
        return "no entries";
    return {};
}

const QuoteLibrary::Person& QuoteLibrary::getPerson (const Entry& e) const
{
    return juce::isPositiveAndBelow (e.person, (int) people.size()) ? people[(size_t) e.person] : unknownPerson;
}

int QuoteLibrary::findEntry (const juce::String& id) const
{
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].id == id)
            return (int) i;
    return -1;
}

int QuoteLibrary::findCategory (const juce::String& id) const
{
    for (size_t i = 0; i < categories.size(); ++i)
        if (categories[i].id == id)
            return (int) i;
    return -1;
}

//==============================================================================
namespace
{
    std::uint64_t freshSeed()
    {
        // random_device alone can be weak on some platforms, so mix in the clock as well.
        std::random_device device;
        const auto clock = (std::uint64_t) std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return ((std::uint64_t) device() << 32 ^ device()) ^ clock;
    }
}

QuotePicker::QuotePicker (const QuoteLibrary& lib) : QuotePicker (lib, freshSeed()) {}

QuotePicker::QuotePicker (const QuoteLibrary& lib, std::uint64_t seed)
    : library (lib), random (seed), decks (lib.getCategories().size())
{
}

int QuotePicker::next (int categoryIndex, int avoid)
{
    const auto& cats = library.getCategories();
    if (! juce::isPositiveAndBelow (categoryIndex, (int) cats.size()))
        return -1;

    const auto& pool = cats[(size_t) categoryIndex].entries;
    if (pool.empty())
        return -1;
    if (pool.size() == 1)
        return pool.front();

    auto& deck = decks[(size_t) categoryIndex];
    if (deck.empty())
    {
        deck = pool;
        std::shuffle (deck.begin(), deck.end(), random);
    }

    // Deal from the back; if the top card is the one on screen, take the one beneath it instead.
    if (deck.back() == avoid && deck.size() > 1)
        std::swap (deck.back(), deck[deck.size() - 2]);

    int pick = deck.back();
    deck.pop_back();

    if (pick == avoid)
    {
        // Only the avoided card was left: start a fresh deck and draw again.
        pick = next (categoryIndex, avoid);
    }
    return pick;
}
