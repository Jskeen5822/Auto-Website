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

## 2. Local run
```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
$env:GITHUB_USERNAME = "Jskeen5822"
$env:GITHUB_TOKEN = "<your_PAT_here>"
python scripts/update_site.py
```
The script writes the generated site to `docs/index.html`. Re-run it whenever you want an instant refresh without waiting for the nightly workflow.

## 3. Continuous updates
- Workflow file: `.github/workflows/update-site.yml`
- Schedule: every day at 05:15 UTC (`cron: "15 5 * * *"`) plus manual `workflow_dispatch` trigger.
- If the generated page changes, the workflow commits with the message `chore: refresh GitHub stats`.

## 4. Customizing
- Tweak the HTML template in `templates/index.html.j2` and styles in `docs/assets/styles.css`.
- Adjust aggregation rules or add new metrics inside `scripts/update_site.py`.
- Add more assets (images, JS) under `docs/` — the workflow will publish anything in that folder.

## Troubleshooting
- **GraphQL errors**: confirm the PAT has the listed scopes and is stored as `GH_STATS_TOKEN`.
- **Workflow cannot push**: make sure the workflow has `contents: write` permission (already set) and no branch protection rules block pushes from GitHub Actions.
- **Pages not updating**: verify GitHub Pages is configured to serve from `main` / `docs` and that `docs/index.html` exists after a workflow run.
