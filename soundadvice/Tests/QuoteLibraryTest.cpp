// Loads the embedded quote database and checks it: every entry has a person, text, a source and a
// category; every category has something in it; and the picker deals a whole category without
// repeating before it reshuffles.

#include "QuoteLibrary.h"

#include <iostream>
#include <set>

namespace
{
    int failures = 0;

    void expect (bool ok, const juce::String& what)
    {
        if (! ok)
        {
            ++failures;
            std::cout << "FAIL: " << what << "\n";
        }
    }
}

int main()
{
    const auto& lib = QuoteLibrary::getEmbedded();
    const auto& entries = lib.getEntries();
    const auto& cats = lib.getCategories();

    std::cout << entries.size() << " entries, " << lib.getPeople().size() << " people, "
              << cats.size() - 1 << " categories\n";

    expect (entries.size() >= 500, "at least 500 entries");
    expect (cats.size() > 1 && cats[0].id == QuoteLibrary::allCategoryId, "first category is Anything");
    expect (cats[0].entries.size() == entries.size(), "Anything holds every entry");

    for (const auto& e : entries)
    {
        const auto& p = lib.getPerson (e);
        expect (p.name.isNotEmpty(), e.id + " has a person");
        expect (e.text.isNotEmpty(), e.id + " has text");
        expect (e.source.url.startsWith ("http"), e.id + " has a source url");
        expect (! e.categories.isEmpty(), e.id + " has a category");
        if (e.verbatim)
            expect (juce::StringArray::fromTokens (e.text, " ", "").size() <= 15, e.id + " quote is 15 words or fewer");
    }

    for (size_t c = 1; c < cats.size(); ++c)
    {
        std::cout << "  " << cats[c].name << ": " << cats[c].entries.size() << "\n";
        expect (cats[c].entries.size() >= 8, cats[c].name + " has at least 8 entries");
    }

    // The picker: one full deal of a category shows each entry exactly once, and never the same twice running.
    QuotePicker picker (lib, 42);
    for (size_t c = 0; c < cats.size(); ++c)
    {
        const auto& pool = cats[c].entries;
        std::set<int> seen;
        int last = -1;
        for (size_t i = 0; i < pool.size(); ++i)
        {
            const auto pick = picker.next ((int) c, last);
            expect (pick != last || pool.size() == 1, cats[c].name + ": no immediate repeat");
            seen.insert (pick);
            last = pick;
        }
        expect (seen.size() == pool.size(), cats[c].name + ": one deal covers the whole category");

        for (int i = 0; i < 50; ++i)
        {
            const auto pick = picker.next ((int) c, last);
            expect (pick != last || pool.size() == 1, cats[c].name + ": no repeat across reshuffles");
            last = pick;
        }
    }

    expect (lib.findEntry (entries.front().id) == 0, "find an entry by id");
    expect (lib.findCategory ("mixing") > 0, "find a category by id");
    expect (picker.next (-1) == -1 && picker.next ((int) cats.size()) == -1, "unknown category picks nothing");

    std::cout << (failures == 0 ? "PASS" : "FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
