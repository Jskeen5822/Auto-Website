"""Generate a GitHub stats website and write it to docs/index.html."""
from __future__ import annotations

import json
import os
import sys
from collections import defaultdict
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, Iterable, List

import requests
from jinja2 import Environment, FileSystemLoader, select_autoescape

ROOT_DIR = Path(__file__).resolve().parent.parent
DOCS_DIR = ROOT_DIR / "docs"
TEMPLATES_DIR = ROOT_DIR / "templates"
OUTPUT_FILE = DOCS_DIR / "index.html"
API_ROOT = "https://api.github.com"
GRAPHQL_ENDPOINT = "https://api.github.com/graphql"


class GitHubAPIError(RuntimeError):
    """Raised when the GitHub API returns an unexpected response."""


def _get_token() -> str:
    token = os.getenv("GITHUB_TOKEN") or os.getenv("GH_STATS_TOKEN")
    if not token:
        raise SystemExit("Missing GITHUB_TOKEN (or GH_STATS_TOKEN) environment variable.")
    return token


def _get_username() -> str:
    username = os.getenv("GITHUB_USERNAME")
    if not username:
        raise SystemExit("Missing GITHUB_USERNAME environment variable.")
    return username


def _headers(token: str) -> Dict[str, str]:
    return {
        "Accept": "application/vnd.github+json",
        "Authorization": f"Bearer {token}",
        "User-Agent": "auto-website-stats-script",
    }


def _request_json(url: str, token: str, *, params: Dict[str, Any] | None = None) -> Any:
    response = requests.get(url, headers=_headers(token), params=params, timeout=20)
    if response.status_code != 200:
        raise GitHubAPIError(f"GET {url} failed: {response.status_code} {response.text}")
    return response.json()


def fetch_profile(username: str, token: str) -> Dict[str, Any]:
    return _request_json(f"{API_ROOT}/users/{username}", token)


def fetch_repositories(username: str, token: str) -> List[Dict[str, Any]]:
    repos: List[Dict[str, Any]] = []
    page = 1
    while True:
        data = _request_json(
            f"{API_ROOT}/users/{username}/repos",
            token,
            params={"per_page": 100, "page": page, "type": "owner", "sort": "updated"},
        )
        if not data:
            break
        repos.extend(data)
        page += 1
    return repos


def fetch_repo_languages(repos: Iterable[Dict[str, Any]], token: str) -> Dict[str, int]:
    language_totals: Dict[str, int] = defaultdict(int)
    for repo in repos:
        languages_url = repo.get("languages_url")
        if not languages_url:
            continue
        response = requests.get(languages_url, headers=_headers(token), timeout=20)
        if response.status_code != 200:
            continue
        for language, bytes_of_code in response.json().items():
            language_totals[language] += bytes_of_code
    return dict(language_totals)


GRAPHQL_QUERY = """
query ($login: String!) {
  user(login: $login) {
    contributionsCollection {
      contributionCalendar {
        totalContributions
        weeks {
          contributionDays {
            date
            contributionCount
          }
        }
      }
    }
  }
}
"""


def fetch_contributions(username: str, token: str) -> Dict[str, Any]:
    payload = {"query": GRAPHQL_QUERY, "variables": {"login": username}}
    response = requests.post(
        GRAPHQL_ENDPOINT,
        headers=_headers(token),
        json=payload,
        timeout=20,
    )
    if response.status_code != 200:
        raise GitHubAPIError(f"GraphQL query failed: {response.status_code} {response.text}")
    payload_json = response.json()
    errors = payload_json.get("errors")
    if errors:
        raise GitHubAPIError(f"GraphQL errors: {errors}")
    return payload_json["data"]["user"]["contributionsCollection"]["contributionCalendar"]


def _calculate_language_summary(language_totals: Dict[str, int]) -> List[Dict[str, Any]]:
    total_bytes = sum(language_totals.values())
    summary: List[Dict[str, Any]] = []
    for language, bytes_of_code in sorted(language_totals.items(), key=lambda item: item[1], reverse=True):
        share = (bytes_of_code / total_bytes * 100) if total_bytes else 0
        summary.append(
            {
                "language": language,
                "bytes": bytes_of_code,
                "share": round(share, 2),
            }
        )
    return summary


def _contribution_trail(contribution_calendar: Dict[str, Any], *, days: int = 120) -> List[Dict[str, Any]]:
    daily_points: List[Dict[str, Any]] = []
    for week in contribution_calendar.get("weeks", []):
        for day in week.get("contributionDays", []):
            daily_points.append(
                {
                    "date": day["date"],
                    "count": day["contributionCount"],
                }
            )
    daily_points.sort(key=lambda item: item["date"])  # ensure chronological order
    return daily_points[-days:]


def _top_repositories(repos: Iterable[Dict[str, Any]], limit: int = 6) -> List[Dict[str, Any]]:
    candidates = [repo for repo in repos if not repo.get("fork")]
    candidates.sort(key=lambda repo: (repo.get("stargazers_count", 0), repo.get("forks_count", 0)), reverse=True)
    top = []
    for repo in candidates[:limit]:
        top.append(
            {
                "name": repo.get("name"),
                "description": repo.get("description") or "",
                "stars": repo.get("stargazers_count", 0),
                "forks": repo.get("forks_count", 0),
                "language": repo.get("language") or "Unknown",
                "url": repo.get("html_url"),
                "updated_at": repo.get("updated_at"),
            }
        )
    return top


def build_context(username: str, token: str) -> Dict[str, Any]:
    profile = fetch_profile(username, token)
    repos = fetch_repositories(username, token)
    own_repos = [repo for repo in repos if not repo.get("fork")]

    language_totals = fetch_repo_languages(own_repos, token)
    contribution_calendar = fetch_contributions(username, token)

    total_stars = sum(repo.get("stargazers_count", 0) for repo in own_repos)
    total_forks = sum(repo.get("forks_count", 0) for repo in own_repos)

    context: Dict[str, Any] = {
        "generated_at": datetime.utcnow().strftime("%Y-%m-%d %H:%M UTC"),
        "profile": {
            "login": profile.get("login"),
            "name": profile.get("name") or profile.get("login"),
            "avatar_url": profile.get("avatar_url"),
            "bio": profile.get("bio") or "",
            "location": profile.get("location") or "",
            "blog": profile.get("blog") or "",
            "followers": profile.get("followers", 0),
            "following": profile.get("following", 0),
        },
        "stats": {
            "public_repos": profile.get("public_repos", 0),
            "followers": profile.get("followers", 0),
            "following": profile.get("following", 0),
            "total_stars": total_stars,
            "total_forks": total_forks,
            "total_contributions": contribution_calendar.get("totalContributions", 0),
        },
        "top_repos": _top_repositories(own_repos),
        "language_summary": _calculate_language_summary(language_totals),
        "contribution_trail": _contribution_trail(contribution_calendar),
    }
    return context


def render_template(context: Dict[str, Any]) -> str:
    env = Environment(
        loader=FileSystemLoader(TEMPLATES_DIR),
        autoescape=select_autoescape(["html", "xml"]),
        trim_blocks=True,
        lstrip_blocks=True,
    )
    template = env.get_template("index.html.j2")
    return template.render(context)


def write_site(html: str) -> None:
    DOCS_DIR.mkdir(parents=True, exist_ok=True)
    OUTPUT_FILE.write_text(html, encoding="utf-8")


def main() -> None:
    token = _get_token()
    username = _get_username()

    context = build_context(username, token)
    html = render_template(context)
    write_site(html)

    print(f"Site updated for {username} -> {OUTPUT_FILE.relative_to(ROOT_DIR)}")


if __name__ == "__main__":
    try:
        main()
    except GitHubAPIError as exc:
        sys.exit(f"GitHub API error: {exc}")
    except requests.RequestException as exc:
        sys.exit(f"Network error: {exc}")
