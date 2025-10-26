package com.autowebsite;

import com.google.gson.Gson;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

import java.io.IOException;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.text.DecimalFormat;
import java.time.OffsetDateTime;
import java.time.ZoneOffset;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;

/**
 * Minimal Java program that mirrors scripts/update_site.py functionality using the GitHub REST and GraphQL APIs.
 */
public final class GitHubStatsApp {
    private static final String API_ROOT = "https://api.github.com";
    private static final String GRAPHQL_ENDPOINT = "https://api.github.com/graphql";
    private static final Path ROOT_DIR = Path.of(".").toAbsolutePath().normalize();
    private static final Path DOCS_INDEX = ROOT_DIR.resolve("docs").resolve("index.html");

    private final HttpClient httpClient;
    private final Gson gson;
    private final String token;
    private final String username;

    private GitHubStatsApp(String token, String username) {
        this.httpClient = HttpClient.newBuilder().build();
        this.gson = new Gson();
        this.token = token;
        this.username = username;
    }

    public static void main(String[] args) throws Exception {
        String token = System.getenv("GITHUB_TOKEN");
        if (token == null || token.isBlank()) {
            token = System.getenv("GH_STATS_TOKEN");
        }
        if (token == null || token.isBlank()) {
            System.err.println("Missing GITHUB_TOKEN or GH_STATS_TOKEN environment variable.");
            System.exit(1);
        }

        String username = System.getenv("GITHUB_USERNAME");
        if (username == null || username.isBlank()) {
            System.err.println("Missing GITHUB_USERNAME environment variable.");
            System.exit(1);
        }

        GitHubStatsApp app = new GitHubStatsApp(token, username);
        Context context = app.buildContext();
        String html = app.renderHtml(context);
        app.writeHtml(html);

        System.out.printf(Locale.US, "Site updated for %s -> %s%n", username, ROOT_DIR.relativize(DOCS_INDEX));
    }

    private Context buildContext() throws IOException, InterruptedException {
        JsonObject profile = fetchProfile();
        List<JsonObject> repositories = fetchRepositories();
        List<JsonObject> ownRepos = repositories.stream()
            .filter(repo -> !repo.get("fork").getAsBoolean())
            .toList();

        Map<String, Long> languageTotals = fetchLanguageTotals(ownRepos);
        ContributionCalendar calendar = fetchContributionCalendar();

        int totalStars = ownRepos.stream().mapToInt(repo -> repo.get("stargazers_count").getAsInt()).sum();
        int totalForks = ownRepos.stream().mapToInt(repo -> repo.get("forks_count").getAsInt()).sum();

        Context context = new Context();
        context.generatedAt = OffsetDateTime.now(ZoneOffset.UTC).format(java.time.format.DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm 'UTC'"));

        Profile p = new Profile();
        p.login = optString(profile, "login");
        p.name = optString(profile, "name", p.login);
        p.avatarUrl = optString(profile, "avatar_url");
        p.bio = optString(profile, "bio");
        p.location = optString(profile, "location");
        p.blog = optString(profile, "blog");
        p.followers = profile.get("followers").getAsInt();
        p.following = profile.get("following").getAsInt();
        context.profile = p;

        Stats stats = new Stats();
        stats.publicRepos = profile.get("public_repos").getAsInt();
        stats.followers = p.followers;
        stats.following = p.following;
        stats.totalStars = totalStars;
        stats.totalForks = totalForks;
        stats.totalContributions = calendar.totalContributions;
        context.stats = stats;

        context.topRepos = buildTopRepos(ownRepos);
        context.languageSummary = buildLanguageSummary(languageTotals);
        context.contributionTrail = calendar.contributionTrail;
        return context;
    }

    private JsonObject fetchProfile() throws IOException, InterruptedException {
        HttpRequest request = requestBuilder(API_ROOT + "/users/" + username).GET().build();
        JsonObject json = sendJson(request);
        if (json == null) {
            throw new IOException("Profile response was empty");
        }
        return json;
    }

    private List<JsonObject> fetchRepositories() throws IOException, InterruptedException {
        List<JsonObject> repos = new ArrayList<>();
        int page = 1;
        while (true) {
            String url = API_ROOT + "/users/" + username + "/repos?per_page=100&type=owner&sort=updated&page=" + page;
            HttpRequest request = requestBuilder(url).GET().build();
            JsonArray data = sendJsonArray(request);
            if (data == null || data.isEmpty()) {
                break;
            }
            for (JsonElement element : data) {
                repos.add(element.getAsJsonObject());
            }
            page += 1;
        }
        return repos;
    }

    private Map<String, Long> fetchLanguageTotals(List<JsonObject> repos) throws IOException, InterruptedException {
        Map<String, Long> totals = new HashMap<>();
        for (JsonObject repo : repos) {
            JsonElement languagesUrlElement = repo.get("languages_url");
            if (languagesUrlElement == null || languagesUrlElement.isJsonNull()) {
                continue;
            }
            String languagesUrl = languagesUrlElement.getAsString();
            HttpRequest request = requestBuilder(languagesUrl).GET().build();
            JsonObject languageMap = sendJson(request);
            if (languageMap == null) {
                continue;
            }
            for (Map.Entry<String, JsonElement> entry : languageMap.entrySet()) {
                long bytes = entry.getValue().getAsLong();
                totals.merge(entry.getKey(), bytes, Long::sum);
            }
        }
        return totals;
    }

    private ContributionCalendar fetchContributionCalendar() throws IOException, InterruptedException {
        JsonObject variables = new JsonObject();
        variables.addProperty("login", username);

        JsonObject payload = new JsonObject();
        payload.addProperty("query", GRAPHQL_QUERY);
        payload.add("variables", variables);

        HttpRequest request = requestBuilder(GRAPHQL_ENDPOINT)
            .POST(HttpRequest.BodyPublishers.ofString(gson.toJson(payload), StandardCharsets.UTF_8))
            .header("Content-Type", "application/json")
            .build();

        JsonObject json = sendJson(request);
        if (json == null) {
            throw new IOException("GraphQL response was empty");
        }
        if (json.has("errors")) {
            throw new IOException("GraphQL error: " + json.get("errors"));
        }
        JsonObject calendar = json
            .getAsJsonObject("data")
            .getAsJsonObject("user")
            .getAsJsonObject("contributionsCollection")
            .getAsJsonObject("contributionCalendar");

        ContributionCalendar result = new ContributionCalendar();
        result.totalContributions = calendar.get("totalContributions").getAsInt();

        List<Map<String, Object>> points = new ArrayList<>();
        JsonArray weeks = calendar.getAsJsonArray("weeks");
        for (JsonElement weekElement : weeks) {
            JsonObject week = weekElement.getAsJsonObject();
            JsonArray days = week.getAsJsonArray("contributionDays");
            for (JsonElement dayElement : days) {
                JsonObject day = dayElement.getAsJsonObject();
                Map<String, Object> point = new LinkedHashMap<>();
                point.put("date", day.get("date").getAsString());
                point.put("count", day.get("contributionCount").getAsInt());
                points.add(point);
            }
        }
        points.sort(Comparator.comparing(point -> point.get("date").toString()));
        int start = Math.max(points.size() - 120, 0);
        result.contributionTrail = points.subList(start, points.size());
        return result;
    }

    private List<TopRepo> buildTopRepos(List<JsonObject> repos) {
        return repos.stream()
            .sorted(Comparator.comparingInt((JsonObject repo) -> repo.get("stargazers_count").getAsInt())
                .thenComparingInt(repo -> repo.get("forks_count").getAsInt())
                .reversed())
            .limit(6)
            .map(repo -> {
                TopRepo top = new TopRepo();
                top.name = optString(repo, "name");
                top.description = optString(repo, "description");
                top.stars = repo.get("stargazers_count").getAsInt();
                top.forks = repo.get("forks_count").getAsInt();
                top.language = optString(repo, "language", "Unknown");
                top.url = optString(repo, "html_url");
                top.updatedAt = optString(repo, "updated_at");
                return top;
            })
            .toList();
    }

    private List<Map<String, Object>> buildLanguageSummary(Map<String, Long> totals) {
        long allBytes = totals.values().stream().mapToLong(Long::longValue).sum();
        DecimalFormat shareFormat = new DecimalFormat("0.00");
        return totals.entrySet().stream()
            .sorted(Map.Entry.<String, Long>comparingByValue().reversed())
            .map(entry -> {
                Map<String, Object> map = new LinkedHashMap<>();
                map.put("language", entry.getKey());
                map.put("bytes", entry.getValue());
                double share = allBytes == 0 ? 0.0 : (entry.getValue() * 100.0) / allBytes;
                map.put("share", Double.parseDouble(shareFormat.format(share)));
                return map;
            })
            .toList();
    }

    private String renderHtml(Context context) {
        String languageJson = gson.toJson(context.languageSummary);
        String contributionJson = gson.toJson(context.contributionTrail);

        StringBuilder html = new StringBuilder();
        html.append("<!DOCTYPE html>\n");
        html.append("<html lang=\"en\">\n");
        html.append("<head>\n");
        html.append("    <meta charset=\"utf-8\">\n");
        html.append("    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
        html.append("    <meta name=\"description\" content=\"Live GitHub statistics for ")
            .append(escape(context.profile.name))
            .append(" (@")
            .append(escape(context.profile.login))
            .append("). Updated daily via GitHub Actions.\">\n");
        html.append("    <title>")
            .append(escape(context.profile.name))
            .append(" · GitHub Insights</title>\n");
        html.append("    <link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n");
        html.append("    <link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>\n");
        html.append("    <link href=\"https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap\" rel=\"stylesheet\">\n");
        html.append("    <link rel=\"stylesheet\" href=\"assets/styles.css\">\n");
        html.append("    <script defer src=\"https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js\"></script>\n");
        html.append("</head>\n<body>\n");

        html.append("    <header class=\"hero\">\n");
        html.append("        <div class=\"hero__avatar\">\n");
        html.append("            <img src=\"").append(escape(context.profile.avatarUrl)).append("\" alt=\"")
            .append(escape(context.profile.name)).append(" avatar\" loading=\"lazy\">\n");
        html.append("        </div>\n");
        html.append("        <div>\n");
        html.append("            <h1>").append(escape(context.profile.name)).append("</h1>\n");
        html.append("            <p class=\"hero__handle\">@")
            .append(escape(context.profile.login)).append("</p>\n");
        if (!context.profile.bio.isBlank()) {
            html.append("            <p class=\"hero__tagline\">")
                .append(escape(context.profile.bio)).append("</p>\n");
        }
        html.append("            <div class=\"hero__meta\">\n");
        if (!context.profile.location.isBlank()) {
            html.append("                <span>📍 ").append(escape(context.profile.location)).append("</span>\n");
        }
        if (!context.profile.blog.isBlank()) {
            html.append("                <span>🔗 <a href=\"")
                .append(escape(context.profile.blog)).append("\" target=\"_blank\" rel=\"noopener\">")
                .append(escape(context.profile.blog)).append("</a></span>\n");
        }
        html.append("            </div>\n");
        html.append("        </div>\n");
        html.append("    </header>\n");

        html.append("    <main>\n");
        html.append(buildStatsGrid(context.stats));
        html.append(buildLanguagePanel(languageJson, context.languageSummary));
        html.append(buildContributionPanel(contributionJson, context.contributionTrail));
        html.append(buildTopReposPanel(context.topRepos));
        html.append("    </main>\n");

        html.append("    <footer class=\"footer\">\n");
        html.append("        <p>Generated on ").append(escape(context.generatedAt)).append(" by an automated workflow.</p>\n");
        html.append("        <p>Source available on <a href=\"https://github.com/").append(escape(context.profile.login))
            .append("/Auto-Website\" target=\"_blank\" rel=\"noopener\">GitHub</a>.</p>\n");
        html.append("    </footer>\n");

        html.append("    <script>\n");
        html.append("    const languageData = ").append(languageJson).append(";\n");
        html.append("    const contributionData = ").append(contributionJson).append(";\n");
        html.append(JAVASCRIPT_HELPERS);
        html.append("    </script>\n");
        html.append("</body>\n</html>\n");
        return html.toString();
    }

    private String buildStatsGrid(Stats stats) {
        return "        <section class=\"stats-grid\" aria-label=\"Key metrics\">\n" +
            statCard("Total Stars", stats.totalStars, "Across public repositories") +
            statCard("Followers", stats.followers, "On GitHub") +
            statCard("Repositories", stats.publicRepos, "Public projects") +
            statCard("Contributions", stats.totalContributions, "Past 365 days") +
            statCard("Total Forks", stats.totalForks, "Across top repos") +
            statCard("Following", stats.following, "Developers tracked") +
            "        </section>\n";
    }

    private String statCard(String title, int value, String hint) {
        return "            <article class=\"stat-card\">\n" +
            "                <h2>" + escape(title) + "</h2>\n" +
            "                <p class=\"stat-card__value\">" + value + "</p>\n" +
            "                <p class=\"stat-card__hint\">" + escape(hint) + "</p>\n" +
            "            </article>\n";
    }

    private String buildLanguagePanel(String languageJson, List<Map<String, Object>> summary) {
        StringBuilder sb = new StringBuilder();
        sb.append("        <section class=\"panel\" aria-label=\"Language breakdown\">\n");
        sb.append("            <div class=\"panel__header\">\n");
        sb.append("                <h2>Language Footprint</h2>\n");
        sb.append("                <p>Distribution across public repositories (top ")
            .append(summary.size()).append(" languages).</p>\n");
        sb.append("            </div>\n");
        sb.append("            <div class=\"panel__body panel__body--chart\">\n");
        if (summary.isEmpty()) {
            sb.append("                <p>No language information available yet.</p>\n");
        } else {
            sb.append("                <canvas id=\"languageChart\" width=\"600\" height=\"320\" role=\"img\" aria-label=\"Language usage chart\"></canvas>\n");
            sb.append("                <table class=\"language-table\">\n");
            sb.append("                    <thead>\n                        <tr>\n                            <th scope=\"col\">Language</th>\n                            <th scope=\"col\">Share</th>\n                            <th scope=\"col\">Source bytes</th>\n                        </tr>\n                    </thead>\n");
            sb.append("                    <tbody>\n");
            for (Map<String, Object> entry : summary) {
                sb.append("                        <tr>\n");
                sb.append("                            <th scope=\"row\">").append(escape(entry.get("language").toString())).append("</th>\n");
                sb.append("                            <td>").append(entry.get("share")).append("%</td>\n");
                sb.append("                            <td>").append(String.format(Locale.US, "%,d", ((Number) entry.get("bytes")).longValue())).append("</td>\n");
                sb.append("                        </tr>\n");
            }
            sb.append("                    </tbody>\n                </table>\n");
        }
        sb.append("            </div>\n");
        sb.append("        </section>\n");
        return sb.toString();
    }

    private String buildContributionPanel(String contributionJson, List<Map<String, Object>> trail) {
        StringBuilder sb = new StringBuilder();
        sb.append("        <section class=\"panel\" aria-label=\"Contribution activity\">\n");
        sb.append("            <div class=\"panel__header\">\n");
        sb.append("                <h2>Contribution Trend</h2>\n");
        sb.append("                <p>Commits, pull requests, issues, and reviews across the last ")
            .append(trail.size()).append(" days.</p>\n");
        sb.append("            </div>\n");
        sb.append("            <div class=\"panel__body panel__body--chart\">\n");
        if (trail.isEmpty()) {
            sb.append("                <p>No contribution data available.</p>\n");
        } else {
            sb.append("                <canvas id=\"contributionChart\" width=\"600\" height=\"320\" role=\"img\" aria-label=\"Contribution activity chart\"></canvas>\n");
        }
        sb.append("            </div>\n");
        sb.append("        </section>\n");
        return sb.toString();
    }

    private String buildTopReposPanel(List<TopRepo> repos) {
        StringBuilder sb = new StringBuilder();
        sb.append("        <section class=\"panel\" aria-label=\"Highlighted repositories\">\n");
        sb.append("            <div class=\"panel__header\">\n");
        sb.append("                <h2>Spotlight Projects</h2>\n");
        sb.append("                <p>Top repositories ranked by stars and forks.</p>\n");
        sb.append("            </div>\n");
        sb.append("            <div class=\"repo-grid\">\n");
        if (repos.isEmpty()) {
            sb.append("                <p>No repositories to show yet. Keep building!</p>\n");
        } else {
            for (TopRepo repo : repos) {
                sb.append("                <article class=\"repo-card\">\n");
                sb.append("                    <header>\n");
                sb.append("                        <h3><a href=\"").append(escape(repo.url)).append("\" target=\"_blank\" rel=\"noopener\">")
                    .append(escape(repo.name)).append("</a></h3>\n");
                sb.append("                        <span class=\"repo-card__language\">")
                    .append(escape(repo.language)).append("</span>\n");
                sb.append("                    </header>\n");
                if (!repo.description.isBlank()) {
                    sb.append("                    <p>").append(escape(repo.description)).append("</p>\n");
                }
                sb.append("                    <footer>\n");
                sb.append("                        <span>⭐ ").append(repo.stars).append("</span>\n");
                sb.append("                        <span>🍴 ").append(repo.forks).append("</span>\n");
                if (!repo.updatedAt.isBlank()) {
                    sb.append("                        <span>🡅 ").append(escape(repo.updatedAt.substring(0, Math.min(10, repo.updatedAt.length())))).append("</span>\n");
                }
                sb.append("                    </footer>\n");
                sb.append("                </article>\n");
            }
        }
        sb.append("            </div>\n");
        sb.append("        </section>\n");
        return sb.toString();
    }

    private void writeHtml(String html) throws IOException {
        Files.createDirectories(DOCS_INDEX.getParent());
        Files.writeString(DOCS_INDEX, html, StandardCharsets.UTF_8);
    }

    private HttpRequest.Builder requestBuilder(String url) {
        return HttpRequest.newBuilder()
            .uri(URI.create(url))
            .header("Accept", "application/vnd.github+json")
            .header("Authorization", "Bearer " + token)
            .header("User-Agent", "auto-website-java-client");
    }

    private JsonObject sendJson(HttpRequest request) throws IOException, InterruptedException {
        HttpResponse<String> response = httpClient.send(request, HttpResponse.BodyHandlers.ofString());
        if (response.statusCode() != 200) {
            throw new IOException("Request failed: " + response.statusCode() + " " + response.body());
        }
        return gson.fromJson(response.body(), JsonObject.class);
    }

    private JsonArray sendJsonArray(HttpRequest request) throws IOException, InterruptedException {
        HttpResponse<String> response = httpClient.send(request, HttpResponse.BodyHandlers.ofString());
        if (response.statusCode() != 200) {
            throw new IOException("Request failed: " + response.statusCode() + " " + response.body());
        }
        return gson.fromJson(response.body(), JsonArray.class);
    }

    private static String optString(JsonObject obj, String key) {
        return optString(obj, key, "");
    }

    private static String optString(JsonObject obj, String key, String defaultValue) {
        JsonElement element = obj.get(key);
        if (element == null || element.isJsonNull()) {
            return defaultValue;
        }
        return element.getAsString();
    }

    private static String escape(String raw) {
        return Objects.toString(raw, "").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;");
    }

    private JsonObject sendJson(HttpRequest.Builder builder) throws IOException, InterruptedException {
        return sendJson(builder.build());
    }

    private static final String GRAPHQL_QUERY = """
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
        """;

    private static final String JAVASCRIPT_HELPERS = """
    const palette = [
        '#5B8FF9',
        '#5AD8A6',
        '#5D7092',
        '#F6BD16',
        '#E8684A',
        '#6DC8EC',
        '#9270CA',
        '#FF9D4D'
    ];

    function buildLanguageChart() {
        if (!languageData.length || !window.Chart) return;
        const ctx = document.getElementById('languageChart');
        const labels = languageData.map(item => item.language);
        const shares = languageData.map(item => item.share);
        new Chart(ctx, {
            type: 'doughnut',
            data: {
                labels,
                datasets: [{
                    data: shares,
                    backgroundColor: palette,
                    borderWidth: 0,
                }]
            },
            options: {
                plugins: {
                    legend: {
                        display: true,
                        position: 'bottom',
                    }
                }
            }
        });
    }

    function buildContributionChart() {
        if (!contributionData.length || !window.Chart) return;
        const ctx = document.getElementById('contributionChart');
        const labels = contributionData.map(point => point.date);
        const counts = contributionData.map(point => point.count);
        new Chart(ctx, {
            type: 'line',
            data: {
                labels,
                datasets: [{
                    label: 'Daily contributions',
                    data: counts,
                    borderColor: '#5B8FF9',
                    backgroundColor: 'rgba(91, 143, 249, 0.2)',
                    tension: 0.3,
                    pointRadius: 0,
                    fill: true,
                }]
            },
            options: {
                scales: {
                    x: {
                        ticks: {
                            maxTicksLimit: 8,
                        }
                    },
                    y: {
                        beginAtZero: true,
                    }
                },
                plugins: {
                    legend: { display: false }
                }
            }
        });
    }

    document.addEventListener('DOMContentLoaded', () => {
        buildLanguageChart();
        buildContributionChart();
    });
    """;

    private static final class Context {
        private String generatedAt;
        private Profile profile;
        private Stats stats;
        private List<TopRepo> topRepos;
        private List<Map<String, Object>> languageSummary;
        private List<Map<String, Object>> contributionTrail;
    }

    private static final class Profile {
        private String login;
        private String name;
        private String avatarUrl;
        private String bio = "";
        private String location = "";
        private String blog = "";
        private int followers;
        private int following;
    }

    private static final class Stats {
        private int publicRepos;
        private int followers;
        private int following;
        private int totalStars;
        private int totalForks;
        private int totalContributions;
    }

    private static final class TopRepo {
        private String name;
        private String description = "";
        private int stars;
        private int forks;
        private String language;
        private String url;
        private String updatedAt = "";
    }

    private static final class ContributionCalendar {
        private int totalContributions;
        private List<Map<String, Object>> contributionTrail;
    }
}
