# Auto-Website

An automated pipeline that builds a GitHub stats dashboard, publishes it with GitHub Pages, and refreshes the content every day without manual work.

## What it does
- Calls the GitHub REST and GraphQL APIs to collect profile, repository, language, and contribution data for `@Jskeen5822`.
- Renders a responsive dashboard (in `docs/index.html`) with charts for language footprint and contribution trends.
- Runs on a nightly GitHub Actions workflow that rebuilds the page and pushes updates when the data changes.

## 1. Prerequisites
1. Create a classic Personal Access Token (PAT) with scopes `read:user`, `repo`, and `public_repo`.
2. Add the PAT as the repository secret `GH_STATS_TOKEN` (Settings → Secrets and variables → Actions → New repository secret).
3. Enable GitHub Pages for this repository (Settings → Pages) and select the `main` branch with `/docs` as the source.
4. Install build requirements if you want to regenerate locally:
	- **Java**: JDK 17+ (Gradle wrapper is included).
	- **C**: A C17 toolchain, CMake 3.20+, and libcurl development headers.

## 2. Local run (Java)
```powershell
cd java
.\gradlew.bat run
```
Environment variables are read from your shell, so set them before invoking Gradle:
```powershell
$env:GITHUB_USERNAME = "Jskeen5822"
$env:GITHUB_TOKEN = "<your_PAT_here>"
```
Use `./gradlew run` on macOS/Linux. The Java client writes the generated site to `../docs/index.html`.

## 3. Local run (C)
```powershell
cmake -S c -B build
cmake --build build --config Release
.\build\Release\github_stats.exe
```
Run these commands from the repository root. Set the same `GITHUB_USERNAME` and `GITHUB_TOKEN` (or `GH_STATS_TOKEN`) variables before running `github_stats`; the executable emits `docs/index.html` from the repository root.

## 4. Continuous updates
- Workflow file: `.github/workflows/update-site.yml`
- Schedule: every day at 05:15 UTC (`cron: "15 5 * * *"`) plus manual `workflow_dispatch` trigger.
- After removing the Python generator, point the workflow at either the Java or C implementation before re-enabling it.
- If the generated page changes, the workflow commits with the message `chore: refresh GitHub stats`.

## 5. Customizing
- Tweak the HTML template in `templates/index.html.j2` and styles in `docs/assets/styles.css`.
- Adjust aggregation or add new metrics in `java/src/main/java/com/autowebsite/GitHubStatsApp.java` or `c/src/github_stats.c` (both generate the same HTML).
- Add more assets (images, JS) under `docs/` — the workflow will publish anything in that folder.

## Troubleshooting
- **GraphQL errors**: confirm the PAT has the listed scopes and is stored as `GH_STATS_TOKEN`.
- **Workflow cannot push**: make sure the workflow has `contents: write` permission (already set) and no branch protection rules block pushes from GitHub Actions.
- **Pages not updating**: verify GitHub Pages is configured to serve from `main` / `docs` and that `docs/index.html` exists after a workflow run.

