#include "AppState.hpp"
#include "HyprlandIPC.hpp"
#include "../helpers/Logger.hpp"
#include "../helpers/OS.hpp"

#include <algorithm>
#include <ranges>
#include <csignal>
#include <fstream>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <functional>
#include <cctype>

#include <hyprutils/string/String.hpp>

using namespace State;

static const std::vector<const char*> IGNORE_DAEMONS = {
    "Xwayland",
};

SP<CAppState> State::state() {
    static auto state = makeShared<CAppState>();
    return state;
}

CApp::CApp(glz::generic::object_t& object) {
    if (object.contains("address"))
        m_address = object["address"].get_string();
    if (object.contains("title"))
        m_title = object["title"].get_string();
    if (object.contains("class"))
        m_class = object["class"].get_string();
    if (object.contains("namespace")) {
        m_class        = object["namespace"].get_string();
        m_alwaysUsePid = true; // layers cant be closewindow'd
    }
    if (object.contains("xwayland"))
        m_xwayland = object["xwayland"].get_boolean();
    if (object.contains("pid"))
        m_pid = sc<int64_t>(object["pid"].get_number());
}

CApp::CApp(const std::string& name, int pid) : m_class(name), m_pid(pid), m_alwaysUsePid(true) {
    ;
}

void CApp::quit() {
    if (!m_alwaysUsePid && (!m_address.empty() || m_pid <= 0)) {
        // for apps that have an address, use closewindow. Some apps don't ask for saving on SIGTERM
        if (m_address.empty()) {
            g_logger->log(LOG_WARN, "CApp::quit: app {} has no address and no valid pid, skipping", m_class);
            return;
        }
        g_logger->log(LOG_TRACE, "CApp::quit: using close for {}", m_class);
        std::string cmd;
        if (State::state()->m_useLua)
            cmd = std::format("/dispatch hl.dsp.window.close({{ window = 'address:{}' }})", m_address);
        else
            cmd = std::format("/dispatch closewindow address:{}", m_address);
        auto ret = HyprlandIPC::getFromSocket(cmd);
        if (!ret)
            g_logger->log(LOG_ERR, "Failed closing window {}: ipc err", m_class);
        else if (*ret != "ok")
            g_logger->log(LOG_ERR, "Failed closing window {}: {}", m_class, *ret);
    } else {
        // SIGTERM with pid
        if (m_pid <= 0) {
            g_logger->log(LOG_WARN, "CApp::quit: app {} has invalid pid {}, skipping SIGTERM", m_class, m_pid);
            return;
        }
        g_logger->log(LOG_TRACE, "CApp::quit: using SIGTERM for {}, pid {}", m_class, m_pid);
        if (::kill(m_pid, SIGTERM) != 0)
            g_logger->log(LOG_ERR, "CApp::quit: signal failed for pid {}, err: {}", m_pid, strerror(errno));
    }
}

void CApp::kill() {
    if (m_pid <= 0) {
        g_logger->log(LOG_TRACE, "Can't kill {}: no pid", m_class);
        return;
    }

    g_logger->log(LOG_TRACE, "CApp::kill: killing {}, pid {}", m_class, m_pid);
    if (::kill(m_pid, SIGKILL) != 0)
        g_logger->log(LOG_ERR, "CApp::quit: signal failed for pid {}, err: {}", m_pid, strerror(errno));
}

bool CApp::appAlive() const {
    if (m_pid <= 0)
        return false;

    if (State::state()->m_dryRun && m_quitSent) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_quitTime).count() / 1000.F;
        float simulatedTime = 2.0F;
        if (m_forceTimeout >= 0.F) {
            simulatedTime = m_forceTimeout;
        }
        if (m_forceTimeout == 0.0F) {
            simulatedTime = 0.5F;
        }
        if (elapsed >= simulatedTime) {
            return false;
        }
    }

    if (::kill(m_pid, 0) == 0)
        return true;

    if (errno == EPERM)
        return true;

    return false;
}

bool CApp::operator==(const glz::generic& object) const {
    if (!object.contains("address"))
        return false;

    return m_address == object["address"].get_string();
}

bool CAppState::init() {

    // detect config provider
    {
        const auto RET = HyprlandIPC::getFromSocket("j/status");
        if (RET) {
            auto jsonRaw = glz::read_json<glz::generic>(*RET);
            if (jsonRaw && jsonRaw->get_object().contains("configProvider")) {
                std::string provider = jsonRaw->get_object()["configProvider"].get_string();
                m_useLua = (provider == "lua");
                g_logger->log(LOG_DEBUG, "Detected config provider: {}", m_useLua ? "lua" : "hyprlang");
            }
        }
    }

    // load config and setup rules
    loadConfig();

    // windows
    {
        const auto RET = HyprlandIPC::getFromSocket("j/clients");

        if (!RET) {
            g_logger->log(LOG_ERR, "Couldn't get clients from socket");
            return false;
        }

        auto jsonRaw = glz::read_json<glz::generic>(*RET);

        if (!jsonRaw) {
            g_logger->log(LOG_ERR, "Socket returned bad data");
            return false;
        }

        auto jsonArr = jsonRaw->get_array();

        m_apps.reserve(jsonArr.size());

        for (auto& el : jsonArr) {
            m_apps.emplace_back(makeUnique<CApp>(el.get_object()));
        }
    }

    // layers
    {
        const auto RET = HyprlandIPC::getFromSocket("j/layers");

        if (!RET) {
            g_logger->log(LOG_ERR, "Couldn't get layers from socket");
            return false;
        }

        auto jsonRaw = glz::read_json<glz::generic>(*RET);

        if (!jsonRaw) {
            g_logger->log(LOG_ERR, "Socket returned bad data");
            return false;
        }

        for (auto& [m, obj] : jsonRaw->get_object()) {
            for (auto& [m2, obj2] : obj["levels"].get_object()) {
                for (auto& el : obj2.get_array()) {
                    m_apps.emplace_back(makeUnique<CApp>(el.get_object()));
                }
            }
        }

        g_logger->log(LOG_DEBUG, "Parsed {} apps from socket", m_apps.size());
    }

    // children of the Hyprland process
    // TODO: make a kernel cgroup in hl. This can miss things.
    // Maybe keep this for BSDs, which don't do cgroups, once we figure out PPid on BSDs.
    {
        const auto INSTANCES = HyprlandIPC::instances();
        const auto HIS       = getenv("HYPRLAND_INSTANCE_SIGNATURE");

        if (HIS && HIS[0] != '\0') {

            const HyprlandIPC::SInstanceData* instance = nullptr;

            for (const auto& I : INSTANCES) {
                if (I.id != HIS)
                    continue;

                instance = &I;
                break;
            }

            if (!instance)
                g_logger->log(LOG_ERR, "Can't get children: no instance??");
            else {
                // get all processes that have a PPid of us
                const auto PROCS = OS::getAllPids();

                for (const auto& pid : PROCS) {

                    // check if child
                    if (OS::ppidOf(pid) != instance->pid)
                        continue;

                    const auto NAME = OS::appNameForPid(pid);

                    if (std::ranges::contains(IGNORE_DAEMONS, NAME))
                        continue;

                    m_apps.emplace_back(makeUnique<CApp>(NAME, pid));
                }
            }

        } else
            g_logger->log(LOG_ERR, "Can't get children: no HIS");
    }

    // Classify all apps
    for (auto& app : m_apps) {
        classifyApp(*app);
    }

    // Determine the unique stages present
    std::vector<int> firstLayers;
    std::vector<int> lastLayers;
    bool hasNormal = false;

    for (const auto& app : m_apps) {
        if (app->m_category == EAppCategory::FIRST) {
            if (std::find(firstLayers.begin(), firstLayers.end(), app->m_layer) == firstLayers.end()) {
                firstLayers.push_back(app->m_layer);
            }
        } else if (app->m_category == EAppCategory::LAST) {
            if (std::find(lastLayers.begin(), lastLayers.end(), app->m_layer) == lastLayers.end()) {
                lastLayers.push_back(app->m_layer);
            }
        } else if (app->m_category == EAppCategory::NORMAL) {
            hasNormal = true;
        }
    }

    std::sort(firstLayers.rbegin(), firstLayers.rend());
    std::sort(lastLayers.begin(), lastLayers.end());

    m_stages.clear();
    for (int l : firstLayers) {
        m_stages.push_back({EAppCategory::FIRST, l});
    }
    if (hasNormal || m_stages.empty()) {
        m_stages.push_back({EAppCategory::NORMAL, 0});
    }
    for (int l : lastLayers) {
        m_stages.push_back({EAppCategory::LAST, l});
    }

    auto getStageIndex = [&](const CApp& app) -> int {
        for (int i = 0; i < (int)m_stages.size(); ++i) {
            if (app.m_category == m_stages[i].category && app.m_layer == m_stages[i].layer) {
                return i;
            }
        }
        return (int)m_stages.size();
    };

    std::stable_sort(m_apps.begin(), m_apps.end(), [&](const UP<CApp>& a, const UP<CApp>& b) {
        int idxA = getStageIndex(*a);
        int idxB = getStageIndex(*b);
        if (idxA != idxB) {
            return idxA < idxB;
        }
        return a->m_class < b->m_class;
    });

    m_stageIndex = 0;
    m_stageStarted = std::chrono::steady_clock::now();

    g_logger->log(LOG_DEBUG, "Initialized {} shutdown stages", m_stages.size());

    // Trigger quit for the first stage
    if (!m_stages.empty()) {
        const auto& currentStage = m_stages[m_stageIndex];
        g_logger->log(LOG_DEBUG, "Starting stage: {}", stageName(currentStage));
        for (const auto& app : m_apps) {
            if (isAppInStage(*app, currentStage)) {
                g_logger->log(LOG_DEBUG, "Starting quit for app in stage {}: {}", stageName(currentStage), app->m_class);
                startAppQuit(*app);
            }
        }
    }

    checkStageTransition();

    return true;
}

const std::vector<UP<CApp>>& CAppState::apps() const {
    return m_apps;
}

float CAppState::secondsPassed() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_started).count() / 1000.F;
}

bool CAppState::updateState() {
    const auto RET = HyprlandIPC::getFromSocket("j/clients");

    if (!RET) {
        g_logger->log(LOG_ERR, "Couldn't get clients from socket");
        return false;
    }

    auto jsonRaw = glz::read_json<glz::generic>(*RET);

    if (!jsonRaw) {
        g_logger->log(LOG_ERR, "Socket returned bad data");
        return false;
    }

    auto       table = jsonRaw->get_array();

    const auto BEFORE = m_apps.size();

    std::erase_if(m_apps, [&table](const auto& e) { return !e->appAlive() && !std::ranges::any_of(table, [&e](const auto& te) { return te == *e; }); });

    // check PIDs
    if (!m_dryRun) {
        for (const auto& app : m_apps) {
            if (!app->appAlive() || app->m_pid <= 0 || app->m_address.empty() /* not a window */ || std::ranges::contains(m_pidsTermedNoWindows, app->m_pid))
                continue;

            const bool HAS_ANY_WINDOWS = std::ranges::any_of(table, [&app](const auto& te) {
                if (!te.contains("pid"))
                    return false;

                return sc<int>(te["pid"].get_number()) == app->m_pid;
            });

            if (HAS_ANY_WINDOWS)
                continue;

            // app has no windows, but is alive. Send a SIGTERM.
            // TODO: maybe make this also repeat every 5s or so?
            m_pidsTermedNoWindows.emplace_back(app->m_pid);

            g_logger->log(LOG_DEBUG, "App {} with pid {} window was closed, but pid is alive. Sending SIGTERM.", app->m_class, app->m_pid);
            kill(app->m_pid, SIGTERM);
        }
    }

    g_logger->log(LOG_DEBUG, "Updated state: apps size {}", m_apps.size());

    // check force timeouts
    for (const auto& app : m_apps) {
        if (app->appAlive() && app->m_quitSent && app->m_forceTimeout >= 0.0F) {
            float elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - app->m_quitTime).count() / 1000.F;
            if (elapsed >= app->m_forceTimeout) {
                g_logger->log(LOG_WARN, "Force-quit timeout exceeded for {} ({}s). Force-killing.", app->m_class, app->m_forceTimeout);
                if (!m_dryRun) {
                    app->kill();
                }
                app->m_forceTimeout = -1.0F;
            }
        }
    }

    checkStageTransition();

    return BEFORE != m_apps.size();
}

void CAppState::killAllApps() const {
    if (m_dryRun) {
        g_logger->log(LOG_TRACE, "CAppState::killAllApps: ignoring, dry run");
        return;
    }

    for (const auto& a : m_apps) {
        a->kill();
    }
}

void CAppState::reexitApps() const {
    if (m_dryRun) {
        g_logger->log(LOG_TRACE, "CAppState::reexitApps: ignoring, dry run");
        return;
    }

    for (const auto& a : m_apps) {
        if (a->m_quitSent) {
            a->quit();
        }
    }
}

static inline std::string localTrim(std::string_view str) {
    auto first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    auto last = str.find_last_not_of(" \t\r\n");
    return std::string(str.substr(first, (last - first + 1)));
}

std::string CAppState::getConfigPath() {
    if (!m_configPathOverride.empty()) {
        return m_configPathOverride;
    }
    std::string configPath;
    const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
    if (xdgConfig && xdgConfig[0] != '\0') {
        configPath = std::string(xdgConfig) + "/hypr/hyprshutdown.conf";
    } else {
        const char* home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            configPath = std::string(home) + "/.config/hypr/hyprshutdown.conf";
        } else {
            struct passwd* pw = ::getpwuid(getuid());
            if (pw) {
                configPath = std::string(pw->pw_dir) + "/.config/hypr/hyprshutdown.conf";
            }
        }
    }
    return configPath;
}

void CAppState::loadConfig() {
    std::string configPath = getConfigPath();
    g_logger->log(LOG_DEBUG, "Attempting to load config from {}", configPath);

    std::ifstream ifs(configPath);
    if (!ifs.good()) {
        g_logger->log(LOG_DEBUG, "Config file not found, using default shutdown settings");
        return;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    // Tokenize
    std::vector<std::string> tokens;
    {
        std::string current;
        bool inQuote = false;
        for (size_t i = 0; i < content.size(); ++i) {
            char c = content[i];
            if (inQuote) {
                if (c == '"') {
                    inQuote = false;
                    tokens.push_back(current);
                    current.clear();
                } else if (c == '\\' && i + 1 < content.size()) {
                    current += content[++i];
                } else {
                    current += c;
                }
            } else {
                if (std::isspace(c)) {
                    if (!current.empty()) {
                        tokens.push_back(current);
                        current.clear();
                    }
                } else if (c == '{' || c == '}' || c == '=' || c == ';' || c == ':') {
                    if (!current.empty()) {
                        tokens.push_back(current);
                        current.clear();
                    }
                    tokens.push_back(std::string(1, c));
                } else if (c == '"') {
                    if (!current.empty()) {
                        tokens.push_back(current);
                        current.clear();
                    }
                    inQuote = true;
                } else if (c == '#') {
                    if (!current.empty()) {
                        tokens.push_back(current);
                        current.clear();
                    }
                    while (i < content.size() && content[i] != '\n') {
                        i++;
                    }
                } else if (c == '/' && i + 1 < content.size() && content[i+1] == '/') {
                    if (!current.empty()) {
                        tokens.push_back(current);
                        current.clear();
                    }
                    while (i < content.size() && content[i] != '\n') {
                        i++;
                    }
                } else {
                    current += c;
                }
            }
        }
        if (!current.empty()) {
            tokens.push_back(current);
        }
    }



    // Block structure
    struct SBlock {
        std::string name;
        std::unordered_map<std::string, std::string> keyValues;
        std::vector<SBlock> subBlocks;
    };

    auto parseBlockHelper = [](auto& self, const std::vector<std::string>& tokens, size_t& idx) -> SBlock {
        SBlock block;
        while (idx < tokens.size()) {
            if (tokens[idx] == "}") {
                idx++; // Consume '}'
                break;
            }

            if (idx + 1 < tokens.size() && tokens[idx + 1] == "{") {
                std::string subName = tokens[idx];
                idx += 2; // Consume Name and '{'
                SBlock sub = self(self, tokens, idx);
                sub.name = subName;
                block.subBlocks.push_back(std::move(sub));
            } else if (idx + 1 < tokens.size() && tokens[idx + 1] == "=") {
                std::string key = tokens[idx];
                std::string val;
                idx += 2; // Consume Key and '='
                if (idx < tokens.size()) {
                    val = tokens[idx];
                    idx++; // Consume Value
                }
                if (idx < tokens.size() && (tokens[idx] == ";" || tokens[idx] == ":")) {
                    idx++; // Consume ';' or ':'
                }
                block.keyValues[key] = val;
            } else {
                idx++;
            }
        }
        return block;
    };

    std::vector<SBlock> topBlocks;
    {
        size_t idx = 0;
        while (idx < tokens.size()) {
            if (idx + 1 < tokens.size() && tokens[idx + 1] == "{") {
                std::string blockName = tokens[idx];
                idx += 2; // Consume Name and '{'
                SBlock top = parseBlockHelper(parseBlockHelper, tokens, idx);
                top.name = blockName;
                topBlocks.push_back(std::move(top));
            } else if (idx + 1 < tokens.size() && tokens[idx + 1] == "=") {
                // Top-level key-values (global settings)
                SBlock globalBlock;
                globalBlock.name = "global";
                while (idx < tokens.size() && idx + 1 < tokens.size() && tokens[idx + 1] == "=") {
                    std::string key = tokens[idx];
                    std::string val;
                    idx += 2; // Consume Key and '='
                    if (idx < tokens.size()) {
                        val = tokens[idx];
                        idx++; // Consume Value
                    }
                    if (idx < tokens.size() && (tokens[idx] == ";" || tokens[idx] == ":")) {
                        idx++; // Consume ';' or ':'
                    }
                    globalBlock.keyValues[key] = val;
                }
                topBlocks.push_back(std::move(globalBlock));
            } else {
                idx++;
            }
        }
    }



    // Process blocks
    m_rules.clear();
    m_defaultLayer = -1;
    m_defaultHidden = false;
    for (const auto& block : topBlocks) {
        if (block.name == "global") {
            if (block.keyValues.contains("systemd_user_exit")) {
                m_systemdUserExit = (block.keyValues.at("systemd_user_exit") == "true" || block.keyValues.at("systemd_user_exit") == "1");
                g_logger->log(LOG_DEBUG, "Config: systemd_user_exit set to {}", m_systemdUserExit);
            }
        } else if (block.name == "default") {
            if (block.keyValues.contains("timeout")) {
                std::string timeoutVal = block.keyValues.at("timeout");
                if (timeoutVal == "unlimited") {
                    m_defaultForceTimeout = -1.0F;
                } else {
                    try {
                        m_defaultForceTimeout = std::stof(timeoutVal);
                    } catch (...) {
                        g_logger->log(LOG_ERR, "Config error: invalid default timeout: '{}'", timeoutVal);
                    }
                }
                g_logger->log(LOG_DEBUG, "Config: default force timeout set to {}", m_defaultForceTimeout);
            }
            if (block.keyValues.contains("layer")) {
                std::string layerVal = block.keyValues.at("layer");
                try {
                    m_defaultLayer = std::stoi(layerVal);
                } catch (...) {
                    g_logger->log(LOG_ERR, "Config error: invalid default layer: '{}'", layerVal);
                }
                g_logger->log(LOG_DEBUG, "Config: default layer set to {}", m_defaultLayer);
            }
            if (block.keyValues.contains("hidden")) {
                m_defaultHidden = (block.keyValues.at("hidden") == "true" || block.keyValues.at("hidden") == "1");
                g_logger->log(LOG_DEBUG, "Config: default hidden set to {}", m_defaultHidden);
            } else if (block.keyValues.contains("hide")) {
                m_defaultHidden = (block.keyValues.at("hide") == "true" || block.keyValues.at("hide") == "1");
                g_logger->log(LOG_DEBUG, "Config: default hidden set to {}", m_defaultHidden);
            }
        } else if (block.name.starts_with("layer_")) {
            int layerNum = 1;
            try {
                layerNum = std::stoi(block.name.substr(6));
            } catch (...) {
                g_logger->log(LOG_ERR, "Config error: invalid layer number: '{}'", block.name);
                continue;
            }

            bool layerHidden = false;
            if (block.keyValues.contains("hidden")) {
                layerHidden = (block.keyValues.at("hidden") == "true" || block.keyValues.at("hidden") == "1");
            } else if (block.keyValues.contains("hide")) {
                layerHidden = (block.keyValues.at("hide") == "true" || block.keyValues.at("hide") == "1");
            }

            for (const auto& sub : block.subBlocks) {
                SShutdownRule rule;
                rule.layer = layerNum;
                rule.forceTimeout = m_defaultForceTimeout;
                rule.hidden = layerHidden;

                if (sub.keyValues.contains("hidden")) {
                    rule.hidden = (sub.keyValues.at("hidden") == "true" || sub.keyValues.at("hidden") == "1");
                } else if (sub.keyValues.contains("hide")) {
                    rule.hidden = (sub.keyValues.at("hide") == "true" || sub.keyValues.at("hide") == "1");
                }

                if (sub.keyValues.contains("timeout")) {
                    std::string timeoutVal = sub.keyValues.at("timeout");
                    if (timeoutVal == "unlimited") {
                        rule.forceTimeout = -1.0F;
                    } else {
                        try {
                            rule.forceTimeout = std::stof(timeoutVal);
                        } catch (...) {
                            g_logger->log(LOG_ERR, "Config error: invalid timeout in sub-block '{}': '{}'", sub.name, timeoutVal);
                        }
                    }
                }

                auto addPattern = [](const std::string& pattern, std::string& patternOut, std::regex& regexOut, bool& flagOut) {
                    patternOut = pattern;
                    try {
                        regexOut = std::regex(pattern, std::regex_constants::ECMAScript | std::regex_constants::nosubs);
                        flagOut = true;
                    } catch (const std::regex_error& e) {
                        g_logger->log(LOG_ERR, "Config error: invalid regex '{}': {}", pattern, e.what());
                    }
                };

                if (sub.keyValues.contains("class")) {
                    addPattern(sub.keyValues.at("class"), rule.classPattern, rule.regexClass, rule.hasClass);
                }
                if (sub.keyValues.contains("title")) {
                    addPattern(sub.keyValues.at("title"), rule.titlePattern, rule.regexTitle, rule.hasTitle);
                }
                if (sub.keyValues.contains("name")) {
                    addPattern(sub.keyValues.at("name"), rule.namePattern, rule.regexName, rule.hasName);
                }
                if (sub.keyValues.contains("cmdline")) {
                    addPattern(sub.keyValues.at("cmdline"), rule.cmdlinePattern, rule.regexCmdline, rule.hasCmdline);
                }
                if (sub.keyValues.contains("path")) {
                    addPattern(sub.keyValues.at("path"), rule.pathPattern, rule.regexPath, rule.hasPath);
                }
                if (sub.keyValues.contains("user")) {
                    addPattern(sub.keyValues.at("user"), rule.userPattern, rule.regexUser, rule.hasUser);
                }
                if (sub.keyValues.contains("pid")) {
                    addPattern(sub.keyValues.at("pid"), rule.pidPattern, rule.regexPid, rule.hasPid);
                }

                bool hasAnyMatch = rule.hasClass || rule.hasTitle || rule.hasName || rule.hasCmdline || rule.hasPath || rule.hasUser || rule.hasPid;
                if (!hasAnyMatch) {
                    addPattern(sub.name, rule.classPattern, rule.regexClass, rule.hasClass);
                }

                g_logger->log(LOG_DEBUG, "Config rule added for sub-block '{}': layer={}, forceTimeout={}s", sub.name, rule.layer, rule.forceTimeout);
                m_rules.push_back(std::move(rule));
            }
        }
    }
}

void CAppState::classifyApp(CApp& app) {
    if (m_defaultLayer == 0) {
        app.m_category = EAppCategory::LAST;
        app.m_layer = 0;
    } else if (m_defaultLayer > 0) {
        app.m_category = EAppCategory::FIRST;
        app.m_layer = m_defaultLayer;
    } else {
        app.m_category = EAppCategory::NORMAL;
        app.m_layer = 0;
    }
    app.m_forceTimeout = m_defaultForceTimeout;
    app.m_hidden = m_defaultHidden;

    for (const auto& rule : m_rules) {
        if (matchRule(app, rule)) {
            if (rule.layer == 0) {
                app.m_category = EAppCategory::LAST;
            } else {
                app.m_category = EAppCategory::FIRST;
            }
            app.m_layer = rule.layer;
            app.m_forceTimeout = rule.forceTimeout;
            app.m_hidden = rule.hidden;
            break;
        }
    }
}

bool CAppState::matchRule(const CApp& app, const SShutdownRule& rule) {
    if (rule.hasClass) {
        try {
            if (!std::regex_match(app.m_class, rule.regexClass) && !std::regex_search(app.m_class, rule.regexClass))
                return false;
        } catch (...) { return false; }
    }
    if (rule.hasTitle) {
        try {
            if (!std::regex_match(app.m_title, rule.regexTitle) && !std::regex_search(app.m_title, rule.regexTitle))
                return false;
        } catch (...) { return false; }
    }
    if (rule.hasName) {
        std::string valueToMatch;
        if (app.m_pid > 0) {
            valueToMatch = OS::appNameForPid(app.m_pid);
        }
        if (valueToMatch.empty()) {
            valueToMatch = app.m_class;
        }
        try {
            if (!std::regex_match(valueToMatch, rule.regexName) && !std::regex_search(valueToMatch, rule.regexName))
                return false;
        } catch (...) { return false; }
    }
    if (rule.hasCmdline) {
        std::string valueToMatch;
        if (app.m_pid > 0) {
            valueToMatch = OS::cmdLineForPid(app.m_pid);
        }
        try {
            if (!std::regex_match(valueToMatch, rule.regexCmdline) && !std::regex_search(valueToMatch, rule.regexCmdline))
                return false;
        } catch (...) { return false; }
    }
    if (rule.hasPath) {
        std::string valueToMatch;
        if (app.m_pid > 0) {
            valueToMatch = OS::exePathForPid(app.m_pid);
        }
        try {
            if (!std::regex_match(valueToMatch, rule.regexPath) && !std::regex_search(valueToMatch, rule.regexPath))
                return false;
        } catch (...) { return false; }
    }
    if (rule.hasUser) {
        std::string valueToMatch;
        if (app.m_pid > 0) {
            valueToMatch = OS::userForPid(app.m_pid);
        }
        try {
            if (!std::regex_match(valueToMatch, rule.regexUser) && !std::regex_search(valueToMatch, rule.regexUser))
                return false;
        } catch (...) { return false; }
    }
    if (rule.hasPid) {
        std::string valueToMatch = std::to_string(app.m_pid);
        try {
            if (!std::regex_match(valueToMatch, rule.regexPid) && !std::regex_search(valueToMatch, rule.regexPid))
                return false;
        } catch (...) { return false; }
    }

    return rule.hasClass || rule.hasTitle || rule.hasName || rule.hasCmdline || rule.hasPath || rule.hasUser || rule.hasPid;
}

void CAppState::checkStageTransition() {
    if (m_stages.empty() || m_stageIndex >= m_stages.size()) {
        return;
    }

    const auto& currentStage = m_stages[m_stageIndex];
    bool hasAliveInCurrentStage = false;
    for (const auto& app : m_apps) {
        if (app->appAlive() && isAppInStage(*app, currentStage)) {
            hasAliveInCurrentStage = true;
            break;
        }
    }

    float stageSecs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_stageStarted).count() / 1000.F;
    float currentTimeout = getTimeoutForStage(currentStage);

    bool timeoutExpired = (stageSecs >= currentTimeout);

    if (!hasAliveInCurrentStage || timeoutExpired) {
        if (timeoutExpired && hasAliveInCurrentStage) {
            g_logger->log(LOG_WARN, "Stage {} timeout expired ({}s). Proceeding to next stage.", stageName(currentStage), currentTimeout);
        }
        advanceStage();
    }
}

void CAppState::startAppQuit(CApp& app) {
    app.m_quitSent = true;
    app.m_quitTime = std::chrono::steady_clock::now();
    if (app.m_forceTimeout == 0.0F) {
        g_logger->log(LOG_DEBUG, "Force-killing app immediately: {}", app.m_class);
        if (!m_dryRun) {
            app.kill();
        }
    } else {
        if (!m_dryRun) {
            app.quit();
        }
    }
}

void CAppState::advanceStage() {
    if (m_stages.empty() || m_stageIndex >= m_stages.size()) {
        return;
    }

    m_stageIndex++;
    if (m_stageIndex >= m_stages.size()) {
        g_logger->log(LOG_DEBUG, "All shutdown stages complete");
        return;
    }

    m_stageStarted = std::chrono::steady_clock::now();
    const auto& nextStage = m_stages[m_stageIndex];
    g_logger->log(LOG_DEBUG, "Transitioning to stage: {}", stageName(nextStage));

    for (const auto& app : m_apps) {
        if (isAppInStage(*app, nextStage)) {
            g_logger->log(LOG_DEBUG, "Starting quit for app in stage {}: {}", stageName(nextStage), app->m_class);
            startAppQuit(*app);
        }
    }

    checkStageTransition();
}

bool CAppState::isAppInStage(const CApp& app, const SShutdownStage& stage) const {
    return app.m_category == stage.category && app.m_layer == stage.layer;
}

float CAppState::getTimeoutForStage(const SShutdownStage& stage) const {
    float maxTimeout = 0.F;
    bool hasUnlimited = false;
    bool hasApps = false;

    for (const auto& app : m_apps) {
        if (app->appAlive() && isAppInStage(*app, stage)) {
            hasApps = true;
            if (app->m_forceTimeout < 0.F) {
                hasUnlimited = true;
            } else {
                maxTimeout = std::max(maxTimeout, app->m_forceTimeout);
            }
        }
    }

    if (hasApps) {
        if (hasUnlimited) {
            return 999999.F; // Effectively unlimited
        }
        return std::max(maxTimeout, 1.0F);
    }

    switch (stage.category) {
        case EAppCategory::FIRST:
            return m_timeoutFirst;
        case EAppCategory::NORMAL:
            return m_timeoutNormal;
        case EAppCategory::LAST:
            return m_timeoutLast;
        default:
            return 0.F;
    }
}

std::string CAppState::stageName(const SShutdownStage& stage) const {
    std::string catName;
    switch (stage.category) {
        case EAppCategory::FIRST:  catName = "FIRST"; break;
        case EAppCategory::NORMAL: catName = "NORMAL"; break;
        case EAppCategory::LAST:   catName = "LAST"; break;
        default:                   catName = "UNKNOWN"; break;
    }
    return std::format("{}(layer {})", catName, stage.layer);
}
