/* AirZip site behaviour: theme, reveals, card spotlight, hero mockup. */
(function () {
  "use strict";

  var root = document.documentElement;
  var reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* --- Theme ------------------------------------------------------------- */
  // The initial theme is applied by an inline script in <head> to avoid a
  // flash of the wrong palette. Here we only wire up the toggle.

  var toggle = document.querySelector(".theme-toggle");
  if (toggle) {
    var labelFor = function (theme) {
      return "Switch to " + (theme === "dark" ? "light" : "dark") + " theme";
    };

    // The markup ships a dark-mode label; correct it for whatever theme actually applied.
    toggle.setAttribute("aria-label", labelFor(root.getAttribute("data-theme")));

    toggle.addEventListener("click", function () {
      var next = root.getAttribute("data-theme") === "dark" ? "light" : "dark";
      root.setAttribute("data-theme", next);
      try { localStorage.setItem("airzip-theme", next); } catch (e) { /* private mode */ }
      toggle.setAttribute("aria-label", "Switch to " + (next === "dark" ? "light" : "dark") + " theme");
    });
  }

  /* --- Mobile nav -------------------------------------------------------- */

  var navToggle = document.querySelector(".nav-toggle");
  var navLinks = document.getElementById("nav-links");

  if (navToggle && navLinks) {
    var isMobile = function () { return window.innerWidth <= 720; };

    var setOpen = function (open) {
      navLinks.hidden = !open;
      navToggle.setAttribute("aria-expanded", String(open));
    };

    var sync = function () { setOpen(!isMobile()); };

    navToggle.addEventListener("click", function () {
      setOpen(navLinks.hidden);
    });

    navLinks.addEventListener("click", function (e) {
      if (e.target.tagName === "A" && isMobile()) setOpen(false);
    });

    window.addEventListener("resize", sync);
    sync();
  }

  /* --- Sticky nav shadow ------------------------------------------------- */

  var nav = document.querySelector(".nav");
  if (nav) {
    var onScroll = function () {
      nav.classList.toggle("is-stuck", window.scrollY > 8);
    };
    window.addEventListener("scroll", onScroll, { passive: true });
    onScroll();
  }

  /* --- Scroll reveal ----------------------------------------------------- */

  var revealables = document.querySelectorAll("[data-reveal]");

  if (reduced || !("IntersectionObserver" in window)) {
    revealables.forEach(function (el) { el.classList.add("is-in"); });
  } else {
    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (!entry.isIntersecting) return;
        // Stagger siblings so grids cascade instead of popping in at once.
        var delay = Number(entry.target.dataset.reveal) || 0;
        setTimeout(function () { entry.target.classList.add("is-in"); }, delay);
        observer.unobserve(entry.target);
      });
    }, { threshold: 0.12, rootMargin: "0px 0px -40px 0px" });

    revealables.forEach(function (el) { observer.observe(el); });
  }

  /* --- Card spotlight ---------------------------------------------------- */

  if (!reduced && window.matchMedia("(hover: hover)").matches) {
    document.querySelectorAll(".card").forEach(function (card) {
      card.addEventListener("pointermove", function (e) {
        var r = card.getBoundingClientRect();
        card.style.setProperty("--mx", (e.clientX - r.left) + "px");
        card.style.setProperty("--my", (e.clientY - r.top) + "px");
      });
    });
  }

  /* --- Copy buttons ------------------------------------------------------ */

  document.querySelectorAll(".copy-btn").forEach(function (btn) {
    btn.addEventListener("click", function () {
      var block = btn.closest(".code");
      var pre = block && block.querySelector("pre");
      if (!pre) return;

      // Drop the "$ " prompt markers so the paste is runnable.
      var clone = pre.cloneNode(true);
      clone.querySelectorAll(".p").forEach(function (p) { p.remove(); });
      var text = clone.textContent.trim();

      navigator.clipboard.writeText(text).then(function () {
        var original = btn.textContent;
        btn.textContent = "Copied";
        btn.classList.add("is-copied");
        setTimeout(function () {
          btn.textContent = original;
          btn.classList.remove("is-copied");
        }, 1600);
      }).catch(function () { /* clipboard blocked — nothing useful to do */ });
    });
  });

  /* --- Hero mockup: replay a real extraction ----------------------------- */

  var toast = document.querySelector("[data-toast]");
  if (toast && !reduced) {
    var elFile  = toast.querySelector(".toast-file");
    var elPct   = toast.querySelector(".toast-pct");
    var elFill  = toast.querySelector(".toast-fill");
    var elLabel = toast.querySelector(".toast-label");
    var elBytes = toast.querySelector(".toast-bytes");

    // Stepped through one at a time as the bar fills, so the preview reads like
    // a real archive being unpacked rather than a single frozen filename.
    var files = [
      "IMG_1102.jpg",
      "IMG_1103.jpg",
      "IMG_1147.jpg",
      "day1/DSC_0091.raw",
      "day1/DSC_0092.raw",
      "clip_0034.mp4",
      "sunset_pano.heic",
      "day2/DSC_0130.raw",
      "day2/DSC_0131.raw",
      "clip_0041.mp4",
      "receipts/hotel.pdf",
      "receipts/flights.pdf",
      "map_route.gpx",
      "packing_list.txt",
      "budget_final.xlsx",
      "playlist.m3u",
      "notes.md",
      "IMG_1220.jpg",
      "IMG_1221.jpg",
      "README.txt"
    ];

    var TOTAL = 520.0;   // MB, matches the sample archive in the screenshots
    var pct = 0;
    var lastIndex = -1;
    var timer;

    var frame = function () {
      pct += 0.7 + Math.random() * 1.7;

      if (pct >= 100) {
        pct = 100;
        elPct.textContent = "100%";
        elFill.style.width = "100%";
        elFile.textContent = "Family_Vacation_v2";
        elLabel.textContent = "Extraction complete";
        elBytes.textContent = TOTAL.toFixed(1) + " / " + TOTAL.toFixed(1) + " MB";
        toast.classList.add("is-done");

        clearInterval(timer);
        setTimeout(function () {           // hold on "done", then loop
          pct = 0;
          lastIndex = -1;
          toast.classList.remove("is-done");
          elLabel.textContent = "Extracting to Family_Vacation_v2...";
          run();
        }, 2600);
        return;
      }

      // Advance the filename only when the bucket actually changes, so each
      // entry in the list gets its own readable moment on screen.
      var i = Math.min(files.length - 1, Math.floor(pct / 100 * files.length));
      if (i !== lastIndex) {
        elFile.textContent = files[i];
        lastIndex = i;
      }

      elPct.textContent = Math.floor(pct) + "%";
      elFill.style.width = pct + "%";
      elBytes.textContent = (TOTAL * pct / 100).toFixed(1) + " / " + TOTAL.toFixed(1) + " MB";
    };

    var run = function () { timer = setInterval(frame, 110); };

    // Only animate while the mockup is actually on screen.
    if ("IntersectionObserver" in window) {
      new IntersectionObserver(function (entries) {
        entries.forEach(function (entry) {
          if (entry.isIntersecting) { if (!timer) run(); }
          else { clearInterval(timer); timer = null; }
        });
      }, { threshold: 0.2 }).observe(toast);
    } else {
      run();
    }
  }

  /* --- Count-up stats ---------------------------------------------------- */

  var stats = document.querySelectorAll("[data-count]");
  if (stats.length && !reduced && "IntersectionObserver" in window) {
    var statObserver = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (!entry.isIntersecting) return;

        var el = entry.target;
        var target = parseFloat(el.dataset.count);
        var decimals = (el.dataset.count.split(".")[1] || "").length;
        var suffix = el.dataset.suffix || "";
        var start = performance.now();
        var DURATION = 1100;

        var tick = function (now) {
          var t = Math.min(1, (now - start) / DURATION);
          var eased = 1 - Math.pow(1 - t, 3);
          el.textContent = (target * eased).toFixed(decimals) + suffix;
          if (t < 1) requestAnimationFrame(tick);
        };

        requestAnimationFrame(tick);
        statObserver.unobserve(el);
      });
    }, { threshold: 0.5 });

    stats.forEach(function (el) { statObserver.observe(el); });
  }

  /* --- Latest release metadata ------------------------------------------- */
  // The download links themselves point at GitHub's /releases/latest/download/
  // redirect, so they never go stale on their own. This only refreshes the
  // version and size *label*, and leaves the server-rendered text if it fails.

  var meta = document.querySelector("[data-release-meta]");
  if (meta && "fetch" in window) {
    fetch("https://api.github.com/repos/abdul-karim-mia/AirZip/releases/latest", {
      headers: { Accept: "application/vnd.github+json" }
    })
      .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
      .then(function (data) {
        var tag = (data.tag_name || "").replace(/^v/, "");
        var exe = (data.assets || []).filter(function (a) {
          return /\.exe$/i.test(a.name);
        })[0];
        if (!tag) return;

        var text = "Version " + tag;
        if (exe && exe.size) text += " · " + (exe.size / 1048576).toFixed(1) + " MB";
        meta.textContent = text;
      })
      .catch(function () { /* rate limited or offline — keep the static label */ });
  }

  /* --- Footer year ------------------------------------------------------- */

  var year = document.querySelector("[data-year]");
  if (year) year.textContent = new Date().getFullYear();
})();
