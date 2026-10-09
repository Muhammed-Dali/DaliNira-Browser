# DaliNira Browser — Official Promotional Website

Production-quality, privacy-focused, security-first static website for [DaliNira Browser](https://github.com/Muhammed-Dali/DaliNira-Browser).

---

## 1. Architectural Principles

This website is engineered according to a **Security-First Architecture** with a strictly minimized attack surface:

- **100% Pure Static Website:** Plain semantic HTML5, pure CSS3, and minimal progressive-enhancement vanilla JavaScript.
- **Zero Backend / Zero Attack Surface:** No PHP, Node.js server, CMS, WordPress, database, or server-side authentication.
- **Zero Tracking & Telemetry:** No Google Analytics, no tracking pixels, no advertising SDKs, no third-party cookies, and no visitor fingerprinting.
- **Zero External Dependencies:** No Google Fonts, no Font Awesome CDN, and no external CDN scripts (cdnjs, jsdelivr, unpkg). System font stack and local SVGs only.
- **Multilingual Support:** English (`/`), Türkçe (`/tr/`), and Arabic (`/ar/` with native RTL layout).
- **Strict Content Security Policy (CSP):** Disallows `unsafe-inline`, `unsafe-eval`, and external origin connections.

---

## 2. Directory Structure

```text
website/
├── index.html           # English landing page (default /)
├── tr/
│   └── index.html       # Türkçe landing page (/tr/)
├── ar/
│   └── index.html       # العربية landing page (/ar/ with dir="rtl")
├── assets/
│   ├── css/
│   │   └── main.css     # Production stylesheet (dark theme, responsive, RTL)
│   ├── js/
│   │   └── main.js      # Minimal vanilla JS (mobile menu, safe DOM copy button)
│   ├── images/          # Product screenshots & official logos
│   └── icons/           # Local SVG icons (shield, download, terminal, check)
├── favicon.svg          # Modern vector SVG favicon
├── favicon.png          # Fallback PNG favicon (32x32)
├── robots.txt           # Search engine crawling rules & sitemap pointer
├── sitemap.xml          # XML sitemap with multilingual hreflang links
├── _headers             # Cloudflare Pages security & caching headers
└── README.md            # Deployment and architecture documentation
```

---

## Local preview

Serve the directory over HTTP, the way GitHub Pages serves it:

```bash
scripts/preview-website.sh        # http://127.0.0.1:8000/  (/tr/, /ar/)
```

Do not preview by opening `website/index.html` as a file. Sandboxed browsers
(Flatpak or Snap builds of Chrome, Firefox and others) receive only that one
file through the desktop document portal, so `assets/` is missing and the page
appears unstyled, without images. Every path in the site is relative, so the
same files work under `http://127.0.0.1:8000/` and on GitHub Pages.

---

## 3. Production Deployment: GitHub Pages

The production site is published at `https://muhammed-dali.github.io/DaliNira-Browser/` by the repository's `website-pages.yml` workflow. The same deployment preserves the historical pacman repository files alongside the website.

### Automatic deployment

1. Push a change under `website/` or run the **Publish Website** workflow manually.
2. The workflow combines the static website with the current pacman repository assets.
3. GitHub Pages deploys the result without a separate build system or external hosting account.

### Custom Domain (Optional)
You can attach a custom domain in the repository's **Settings → Pages** screen without changing the static site structure.

### Security Headers Configured (`_headers`):
- `Content-Security-Policy`: Restricts scripts, styles, and assets to `'self'`.
- `X-Content-Type-Options: nosniff`
- `X-Frame-Options: DENY`
- `Referrer-Policy: no-referrer`
- `Permissions-Policy`: Shuts down camera, microphone, geolocation, usb, payments, etc.
- `Cross-Origin-Opener-Policy: same-origin`
- `Cross-Origin-Resource-Policy: same-origin`
- `Strict-Transport-Security: max-age=31536000; includeSubDomains; preload`

---

## 4. Security headers

> **Technical Security Note on GitHub Pages:**  
> Unlike Cloudflare Pages, standard GitHub Pages does not evaluate the `_headers` file to emit custom HTTP response headers. To safeguard the site on GitHub Pages, equivalent `<meta http-equiv="Content-Security-Policy">` and `<meta name="referrer" content="no-referrer">` tags are already embedded directly within every HTML file.

---

## 5. GitHub Repository Website Configuration

After deployment, update the GitHub repository's **About** panel:

1. Navigate to `https://github.com/Muhammed-Dali/DaliNira-Browser`.
2. Click the gear icon next to **About** in the right sidebar.
3. In the **Website** field, enter the production URL:
   ```text
   https://muhammed-dali.github.io/DaliNira-Browser/
   ```
4. Save changes.
