# AirZip landing page

Static marketing site for [AirZip](https://github.com/abdul-karim-mia/AirZip). No build
step, no framework, no dependencies — three HTML files, one stylesheet, one script.

## Structure

```
index.html            Home — hero, features, formats, comparison, FAQ
features.html         Deep dive — extraction, UI, security, technical design, limits
installation.html     Store, portable .exe, CLI reference, uninstall, build, troubleshooting
robots.txt
sitemap.xml
vercel.json           Cache headers for /assets, basic security headers
assets/
  css/styles.css      Design system — tokens, components, animations
  js/main.js          Theme toggle, scroll reveals, card spotlight, hero mockup
  favicon.ico
  img/
    logo.png          512px, generated from ../icon.png
    apple-touch-icon.png
    favicon-64.png
    shots/            Cropped from ../store-listing/screenshots
```

## Local preview

```bash
python -m http.server 8000
```

Then open <http://localhost:8000>. Links are root-relative, so serve from this directory
rather than opening the files directly.

## Deploy to Vercel

1. Push the repo to GitHub.
2. [vercel.com](https://vercel.com) → **Add New → Project** → import `abdul-karim-mia/AirZip`.
3. Set **Root Directory** to `website`.
4. Framework preset: **Other**. Leave build and output settings empty.
5. Deploy.

Every push to `main` redeploys automatically.

## Analytics

Each page loads two Vercel scripts before `</body>`:

| Script | What it measures |
|---|---|
| `/_vercel/insights/script.js` | Web Analytics — page views, referrers, countries |
| `/_vercel/speed-insights/script.js` | Speed Insights — Core Web Vitals from real visitors |

Both use a queue stub (`window.va` / `window.si`) so calls made before the script resolves
are not lost.

**The script tags alone do nothing.** Each product must be switched on separately in the
Vercel dashboard — **Analytics** and **Speed Insights** in the sidebar, **Enable** on each
— and then redeployed. Until that happens, and on any local preview, both requests 404
harmlessly and the pages are unaffected.

Vercel also provisions a project-specific `/<unique-path>/script.js` route for each, which
survives ad blockers better than the `/_vercel/` paths. If the dashboard shows those paths
after enabling, swap them into all three HTML files.

To confirm they work once deployed, look in the Network tab for a `view` request
(Analytics) and check that the Speed Insights script loads.

Speed Insights bills by data point beyond the free allowance, so keep an eye on
[usage](https://vercel.com/docs/speed-insights/managing-usage) if traffic grows.

## Caching

`vercel.json` caches `/assets/img/*` for a year as `immutable`, but CSS and JS
**must revalidate** on every request. Those filenames are not content-hashed, so caching
them long-term would freeze visitors on a stale stylesheet — which is exactly what happens
if you get it wrong.

The `?v=N` query on the `styles.css` and `main.js` links is a second line of defence
against intermediary caches. **Bump `N` in all three HTML files whenever you ship a
visible CSS or JS change.**

## Design notes

- **Theme.** Dark by default, follows `prefers-color-scheme`, and the toggle persists to
  `localStorage` under `airzip-theme`. An inline script in each `<head>` applies the theme
  before first paint so there is no flash of the wrong palette.
- **The hero mockup** is an HTML/CSS recreation of AirZip's real progress window, animated
  by `main.js`. Its palette is tokenised (`--toast-*`) and tracks the site theme, mirroring
  how the app follows the Windows light/dark setting. Colours are sampled from the
  screenshots in `store-listing/`.
- **Motion** is gated behind `prefers-reduced-motion`. With motion reduced, reveals resolve
  immediately and the mockup holds a static mid-extraction state.
- **No JS required** for content — scroll reveals are the only thing that needs it, and the
  mockup renders a sensible static state without it.

## Regenerating image assets

Requires ImageMagick. Run from the repository root:

```bash
magick icon.png -resize 512x512 -strip website/assets/img/logo.png
magick icon.png -resize 180x180 -strip website/assets/img/apple-touch-icon.png
magick icon.png -resize 64x64  -strip website/assets/img/favicon-64.png
magick icon.png -define icon:auto-resize=48,32,16 website/assets/favicon.ico
```

## Content accuracy

The site states plainly which formats are verified versus wired-up-but-untested, and lists
known limitations. Keep that in sync with the root `README.md` — claiming untested formats
work is the one change that would make this site dishonest.
