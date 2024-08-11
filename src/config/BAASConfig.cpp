//
// Created by pc on 2024/4/12.
//
#include "config/BAASConfig.h"

#include <string>

#include "BAASExceptions.h"
#include "BAASGlobals.h"
using namespace std;
using json = nlohmann::json;

BAASConfig::BAASConfig(const json &j, BAASLogger *logger) {
    assert(logger != nullptr);
    this->logger = logger;
    config = j;
}


BAASConfig::BAASConfig(const string &path, BAASLogger *logger) {
    assert(logger != nullptr);
    this->logger = logger;
    this->path = path;
    if(filesystem::exists(path)) {
        try {
            std::lock_guard<std::mutex> lock(mtx);
            std::ifstream file(path);
            config = json::parse(file);
        }
        catch (json::parse_error &e) {
            logger->BAASError("Config file [ " + path + " ] parse error : " + e.what());
            throw ValueError("Config file parse error : [ " + path + " ] : " + e.what());
        }
    }
    else throw PathError("Config file not exist : [ " + path + " ].");
}


BAASConfig::BAASConfig(const int config_type) {
    path = BAAS_PROJECT_DIR + "\\resource";
    switch (config_type) {
        case CONFIG_TYPE_STATIC_CONFIG:
            path += "\\static.json";
            break;
        case CONFIG_TYPE_DEFAULT_CONFIG:
            path += "\\default_config.json";
            break;
        case CONFIG_TYPE_CONFIG_NAME_CHANGE:
            path += "\\config_name_change.json";
            break;
        default:
            throw ValueError("Invalid config type : [ " + to_string(config_type) + " ].");
    }
    if(filesystem::exists(path)) {
        try {
            std::lock_guard<std::mutex> lock(mtx);
            std::ifstream file(path);
            config = json::parse(file);
            logger = (BAASLogger*)(BAASGlobalLogger);
            BAASGlobalLogger->BAASInfo("Config file [ " + path + " ] loaded successfully.");
        }
        catch (json::parse_error &e) {
            BAASGlobalLogger->BAASError("Config file [ " + path + " ] parse error : " + e.what());
            throw ValueError("Config file parse error : [ " + path + " ] : " + e.what());
        }
    }
    else throw PathError("Config file not exist : [ " + path + " ].");
}

BAASConfig::BAASConfig(const string &path) {
    vector<string> split;
    BAASUtil::stringSplit(path, "\\", split);
    assert(split.size() == 2);
    config_name = split[0];
    logger = BAASLogger::get(config_name);

    if(not BAASUtil::endsWith(split[1], ".json")) throw ValueError("Config file must be a json file : [ " + path + " ]");

    this->path = BAAS_CONFIG_DIR + "\\" + path;
    if(filesystem::exists(this->path)) {
        modified = json::array({});
        // init modify history
        // save in "output/{date}/config_modify_history/name/config.json" date is generated by global logger
        modify_history_path = GlobalLogger::get_folder_path() + "\\" + "config_modify_history";
        if (!std::filesystem::exists(modify_history_path)) {
            logger->BAASInfo("Make Config Modify History Dir : [ " + modify_history_path + " ].");
            std::filesystem::create_directory(modify_history_path);
        }
        modify_history_path += "\\" + split[0];
        std::filesystem::create_directory(modify_history_path);
        modify_history_path += "\\" + split[1];
        std::ofstream modify_record_file(modify_history_path);
        modify_record_file << json::object({}).dump(4);
        modify_record_file.close();
        try {
            std::lock_guard<std::mutex> lock(mtx);
            std::ifstream file(this->path);
            config = json::parse(file);
            logger->BAASInfo("Config file [ " + this->path + " ] loaded successfully.");
        }
        catch (json::parse_error &e) {
            logger->BAASError("Config file [ " + this->path + " ] parse error : " + e.what());
            throw ValueError("Config file parse error : [ " + this->path + " ] : " + e.what());
        }
        preProcessValue();
        save();
    }
    else throw PathError("Config file not exist : [ " + this->path + " ].");
}

void BAASConfig::load() {
    std::lock_guard<std::mutex> lock(mtx);
    std::ifstream file(path);
    config = json::parse(file);
    preProcessValue();
    save_modify_history();
}

void BAASConfig::save() {
    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream file(path);
    file << config.dump(4);
    file.close();
    save_modify_history();
}

void BAASConfig::preprocess(string& jp, json &target) {
    switch(target.type()) {
        case json::value_t::object:
            for(auto it = target.begin(); it != target.end(); it++) {
                int jp_len = int(jp.size());
                jp += '/' + it.key();
                preprocess(jp, *it);
                jp.resize(jp_len);
            }
            break;
        case json::value_t::array:
            for(auto it = target.begin(); it != target.end(); it++) {
                int jp_len = int(jp.size());
                jp += '/' + to_string(it - target.begin());
                preprocess(jp, *it);
                jp.resize(jp_len);
            }
            break;
        case json::value_t::string: {
            string str = target.template get<string>();
            if (str.empty()) return;
            if (str == "true") {
                target = true;
                modified.push_back({{"op",    "replace"},
                                    {"path",  jp},
                                    {"value", target}});
            } else if (str == "false") {
                target = false;
                modified.push_back({{"op",    "replace"},
                                    {"path",  jp},
                                    {"value", target}});
            } else if (str == "None" or str == "null") {
                target = nullptr;
                modified.push_back({{"op",    "replace"},
                                    {"path",  jp},
                                    {"value", target}});
            } else {
                // try to convert to number
                if (BAASUtil::allNumberChar(str)) {
                    if (str.find('.') == string::npos) {
                        try {
                            target = stoi(str);
                            modified.push_back({{"op",    "replace"},
                                                {"path",  jp},
                                                {"value", target}});
                        } catch (invalid_argument &e) {}
                    } else {
                        try {
                            target = stod(str);
                            modified.push_back({{"op",    "replace"},
                                                {"path",  jp},
                                                {"value", target}});
                        }
                        catch (invalid_argument &e) {}
                    }
                }
            }
            break;
        }
        case json::value_t::null:
        case json::value_t::boolean:
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float:
        case json::value_t::binary:
        case json::value_t::discarded:
        default: {
            return;
        }
    }
}



void BAASConfig::remove(const string &key) {
    assert(!key.empty());
    std::lock_guard<std::mutex> lock(mtx);
    if(!count(key.begin(), key.end(), '/')) {
        auto it = findByKey(key);
        config.erase(it);
    }
    else {
        vector<string> keys;
        BAASUtil::stringSplit(key, '/', keys);
        json &tar = config;
        int siz = int(keys.size()) - 1;
        for(int i = 0; i <= siz-1 ; ++i) {
            if(tar.is_object()) {
                auto it = tar.find(keys[i]);
                if(it != tar.end())tar = *it;
                else throwKeyError("Key : [ " + key + " ] not fount.");
            }
            else if(tar.is_array()) {
                try{
                    int idx = stoi(keys[i]);
                    tar = tar.at(idx);
                }catch(exception &e) {throwKeyError(e.what());}
            }else throwKeyError("key : [ " + key + " ] not found.");

        }

        if(tar.is_object() ) {
            if(tar.contains(keys[siz])) tar.erase(keys[siz]);
            else throwKeyError("key : [ " + key + " ] not found.");
        }

        else if(tar.is_array()) {
            try{
                int idx = stoi(keys[siz]);
                tar.erase(idx);
            }catch(exception &e) {throwKeyError(e.what());}
        }else throwKeyError("key : [ " + key + " ] not found.");
    }
    modified.push_back({{"op", "remove"}, {"path", key}});
}

void BAASConfig::show() {
    std::lock_guard<std::mutex> lock(mtx);
    cout << config.dump(4) << endl;
}

void BAASConfig::show_modify_history() {
    std::lock_guard<std::mutex> lock(mtx);
    cout << modified.dump(4) << endl;
}

void BAASConfig::diff(json &j, nlohmann::json& result) {
    result = json::diff(j, config);
}

void BAASConfig::clear() noexcept{
    std::lock_guard<std::mutex> lock(mtx);
    config.clear();
}

void BAASConfig::replace_all(json &new_config) {
    std::lock_guard<std::mutex> lock(mtx);
    config = new_config;
}

void BAASConfig::my_flatten() {
    std::lock_guard<std::mutex> lock(mtx);
    json res = json::object({});
    string jp;
    modified.push_back({{"op", "flatten"}});
    flatten(jp, config, res);
    config = res;
}

void BAASConfig::flatten(string &jp, json &tar, json &result) {
    switch (tar.type()) {
        case json::value_t::object:
            if (tar.empty()) {
                result[jp] = nullptr;
                break;
            }
            else {
                for(auto it = tar.begin(); it != tar.end(); it++) {
                    int jp_len = int(jp.size());
                    jp += '/' + it.key();
                    flatten(jp, *it, result);
                    jp.resize(jp_len);
                }
            }
            break;
        case json::value_t::array:
        case json::value_t::null:
        case json::value_t::string:
        case json::value_t::boolean:
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float:
        case json::value_t::binary:
        case json::value_t::discarded:
        default:
            result[jp] = tar;
    }
}

void BAASConfig::my_unflatten() {
    modified.push_back({{"op", "unflatten"}});
    unflatten(config);
}

void BAASConfig::unflatten(json &value) {
    std::lock_guard<std::mutex> lock(mtx);
    json result = json::object({});
    for(auto it = value.begin(); it != value.end(); it++) {
        assert(it.key().front() == '/');
        assert(!it->is_object());
        result[json::json_pointer(it.key())] = it.value();
    }
    value = result;
}



BAASConfig *config_name_change = nullptr;

