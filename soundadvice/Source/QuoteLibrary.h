#pragma once

#include <juce_core/juce_core.h>

#include <random>
#include <vector>

/** The quote database: people, entries and categories, parsed from the embedded quotes.json.
    Read-only after loading, so one copy is shared by every instance of the plug-in. */
class QuoteLibrary
{
public:
    struct Person
    {
        juce::String name, role, credits, imdb;
    };

    struct Source
    {
        juce::String title, publication, url;
        int year = 0;
    };

    struct Entry
    {
        juce::String id;
        int person = -1;
        bool verbatim = true;          // true: the person's own words; false: a technique or story retold
        juce::String text;
        juce::String context;          // what a quote is about (a film, show or game), shown above it
        juce::StringArray categories;  // category ids, primary first
        Source source;
    };

    struct Category
    {
        juce::String id, name;
        std::vector<int> entries;      // indices into entries()
    };

    /** The id of the catch-all category that holds every entry. It is always categories()[0]. */
    static constexpr const char* allCategoryId = "all";

    /** The library built into the plug-in. Loaded on first use. */
    static const QuoteLibrary& getEmbedded();

    /** Parses a database. Returns an empty string on success, otherwise what was wrong. */
    juce::String loadFromJson (const juce::String& jsonText);

    const std::vector<Person>& getPeople() const noexcept { return people; }
    const std::vector<Entry>& getEntries() const noexcept { return entries; }
    const std::vector<Category>& getCategories() const noexcept { return categories; }

    const Person& getPerson (const Entry& e) const;

    int findEntry (const juce::String& id) const;
    int findCategory (const juce::String& id) const;

private:
    std::vector<Person> people;
    std::vector<Entry> entries;
    std::vector<Category> categories;
    Person unknownPerson;
};

/** Picks entries at random without repeats: each category deals from its own shuffled deck,
    and the deck is reshuffled only once every entry in it has been shown. */
class QuotePicker
{
public:
    /** Seeded from std::random_device, so every launch deals in a different order. */
    explicit QuotePicker (const QuoteLibrary& library);
    /** A fixed seed, for repeatable tests. */
    QuotePicker (const QuoteLibrary& library, std::uint64_t seed);

    /** The next entry index for a category, never `avoid` unless the category has only one entry.
        Returns -1 for an unknown or empty category. */
    int next (int categoryIndex, int avoid = -1);

private:
    const QuoteLibrary& library;
    std::mt19937_64 random;
    std::vector<std::vector<int>> decks;
};
