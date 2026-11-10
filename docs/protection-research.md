# Can NX2navi be protected from copying, including by AI tools?

Research note, 2026-11-07. Not legal advice.

**Short answer.** Nothing stops a person with read access from copying code or pasting it into an AI tool. You can limit who gets access, set legal terms, and opt out of cooperative crawlers. Ideas are free to reuse; only patents cover them, and they are impractical here.

## Facts in this repo that change the answer

- `LICENSE` names **Valiantsin Dzerakh**, not chrg6. The local clone is shallow; its root commit is by that author, and the remote is `valentderah/totk-pc-recomp`, which the GitHub page showed as **public, MIT, about 5.5k stars and 459 forks**. Copies already made under MIT stay MIT. Only the author of a piece of code can relicense it, so chrg6 can relicense their own commits (23 of 24 here), not the base code.
- `AGENTS.md` says "This repository is public"; update it once the decision is made.
- `THIRD_PARTY_NOTICES.md` lists about 40 components under MIT, BSD, zlib, ISC and Apache-2.0. 44 of 112 `runtime/*.c` files carry copyright lines.

## 1. Technical limits

| Measure | What it does | Limit |
|---|---|---|
| Private repo | Strongest control. [GitHub's terms](https://docs.github.com/en/site-policy/github-terms/github-terms-of-service) treat private content as confidential, with narrow staff access | Anyone invited can clone and paste it anywhere |
| GitHub AI settings | Since 24 Apr 2026, Copilot Free/Pro/Pro+ interaction data trains models unless you opt out at `github.com/settings/copilot/features`. Private content at rest is reportedly not used; code processed while Copilot is active in it is ([GitHub blog](https://github.blog/news-insights/company-news/updates-to-github-copilot-interaction-data-usage-policy/); details from secondary reports) | Copilot only. For public repos the terms (D.4) license your content to GitHub for AI training, and the opt-out excludes public repo content (J.3) |
| robots.txt | You cannot edit github.com's. [GitHub's](https://github.com/robots.txt) lets GPTBot, ClaudeBot and Claude-User crawl with path limits such as `/copilot/`; only Bytespider is blocked. No per-repo opt-out | Only for sites you host. Voluntary |
| Crawler tokens | [Anthropic](https://support.claude.com/en/articles/8896518): ClaudeBot (training), Claude-SearchBot, Claude-User; [OpenAI](https://developers.openai.com/api/docs/bots): GPTBot (training), OAI-SearchBot, ChatGPT-User (robots.txt "may not apply"); [Google-Extended](https://developers.google.com/search/docs/crawling-indexing/google-common-crawlers): Gemini training and grounding, not Search | Forward-looking; each bot named |

**Cannot be prevented:** a clone pasted into an AI tool, a local assistant reading files, a collaborator's own settings, a user-triggered fetch of a public URL, or any scraper that ignores robots.txt. Notices in `AGENTS.md` or file headers are signals, not controls.

## 2. Legal options

[17 U.S.C. 102(b)](https://www.law.cornell.edu/uscode/text/17/102): copyright never extends to "any idea, procedure, process, system, method of operation, concept, principle, or discovery". A clean rewrite of our approach is legal.

| Licence | Effect | Enforceability |
|---|---|---|
| MIT (now) | Anyone may copy, sell, relicense; must keep the notice | Nothing to enforce beyond the notice |
| GPL / AGPL | Copiers must publish derived source under the same terms; AGPL covers network use. Does not stop copying | Enforceable by the holder; fits only if you want sharing |
| PolyForm Noncommercial | Source-available; bars commercial use | Works as a copyright licence; not open source |
| BSL 1.1 | Limits production use, then [converts to GPL-compatible](https://mariadb.com/bsl11/) within 4 years; "not an Open Source license" | Built for companies; poor fit |
| All rights reserved | Default under copyright; fine while private | Strong in law, but no one else may use or contribute |
| "No AI training" clause | Custom wording, no standard text; breaks the [Open Source Definition](https://opensource.org/osd) rule against field-of-use limits | Untested. US district courts found AI training fair use on the facts in 2025 (Bartz v. Anthropic, Kadrey v. Meta; [review](https://copyrightalliance.org/ai-copyright-case-developments-2025/)), and in [Doe v. GitHub](https://www.gibsondunn.com/ninth-circuit-clarifies-limits-of-dmca-liability-for-ai-generated-code/) (9th Cir., Sept 2026) only the contract claims survive. I found no appeals ruling on fair use |

Enforcement means finding a copy, proving copying and paying a lawyer. A DMCA takedown is the cheap tool for copies on GitHub.

**Patents** are the only protection for ideas. Reported costs: USPTO provisional $65 to $325, software drafting and filing about $8,000 to $15,000, the full path $15,000 to $30,000 or more. They need filing before public disclosure, and published upstream work is prior art. From general knowledge, not checked here: software eligibility after *Alice* is shaky. Not worth it.

## 3. What can and cannot be relicensed

**Cannot:** every row of `THIRD_PARTY_NOTICES.md`. This covers `tools/vendor/` (unmodified), Ryujinx's shader translator, `tools/xlate/` (its `LICENSE` is MIT, jhaizhou) and files adapted from MacNativeGames, LASRecomp and others. They stay MIT, zlib or BSD with their notices. zlib code (`runtime/nvn_tex.c`) must be marked if altered. Stricter terms can cover only our own additions; anyone can still take the MIT parts from their upstream projects.

**Translator:** pinned MIT Ryujinx code plus patches. Patch lines we wrote (nine of ten commits touching `tools/xlate/patches/` are chrg6's; per-patch authorship is not recorded) can carry our terms. The patched translator still ships under MIT with Ryujinx's notice, and the upstream stays free to everyone.

**Can, if the author agrees or it is chrg6's alone:** new files such as `runtime/widescreen.c` and `recomp/patches-v111.txt`, and the Ocarina port docs.

**Gap:** the TOTK-Ultrawide row is GPL with no URL or version; keep it data-only and record the source.

## 4. Recommendations, ranked

1. **Keep the repo private**, invite only trusted people, turn off the Copilot training opt-in in your account and ask collaborators to do the same. Free; the only real control.
2. **Settle ownership with Dzerakh in writing**: who owns what, and whether a restrictive licence on the additions is acceptable. Without it you can relicense only your own work.
3. **Choose a licence for our additions** (all rights reserved while private; PolyForm Noncommercial if shared), via per-file SPDX headers and a `LICENSE` that states the split. One short lawyer review.
4. **Add a notice** in `README.md` reserving rights and stating that AI training is not permitted. A legal signal, not protection.
5. **Add robots.txt** blocks for ClaudeBot, GPTBot and Google-Extended on any site you host.
6. **Skip patents.** The practical moat is staying ahead.

Expect deterrence and a clean legal footing, not prevention.
