# AirZip Landing Page

A modern, responsive landing page for AirZip — built with HTML5, CSS3, and automatic light/dark theme support.

## Files

- `index.html` — Main landing page
- `styles.css` — Responsive styling with theme support
- `package.json` — Project metadata
- `vercel.json` — Vercel deployment config

## Local Development

To preview the site locally:

```bash
# Using Python
python -m http.server 8000

# Or using Node (npx)
npx http-server
```

Then open `http://localhost:8000` in your browser.

## Deploy to Vercel

1. Push the `website` folder to GitHub (if not already there)
2. Go to [vercel.com](https://vercel.com)
3. Click "New Project"
4. Import your GitHub repo
5. Set the root directory to `website`
6. Deploy

Vercel will automatically:
- Deploy on every push
- Assign a `.vercel.app` domain
- Provide HTTPS
- Handle caching and CDN

## Features

✅ Modern card-based design
✅ Light/dark theme (follows system settings)
✅ Fully responsive (mobile-friendly)
✅ Zero JavaScript dependencies
✅ Fast load times (~50KB total)
✅ SEO-friendly HTML5

## Customization

Edit `index.html` to change content and `styles.css` to adjust colors/typography.
