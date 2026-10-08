/*
 * Stoatworks Labs - About window data for Fiche.
 *
 * PROVISIONAL HAND COPY, adapted from patchwork's generated header on
 * 2026-10-08, before this repo is registered. Registration replaces it with the
 * output of stoatworks-backend/scripts/sync-about.py, generated from the
 * website's projects.json. `guide` is empty because no user guide exists yet, so
 * the About block has no "User guide" button (see StoatworksAboutLinks.h).
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Fiche";
    inline constexpr auto slug = "fiche";
    inline constexpr auto hook = "Browsing a microfiche reader, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/fiche/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/fiche";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
