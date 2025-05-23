#include <cstring>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cinttypes>
#include <string> // Para std::string y strlen
#include <cerrno> // Para strerror
#include <vector> // Para la lista de prefijos

#include "hack.h"
#include "zygisk.hpp"
#include "log.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

class MyModule : public zygisk::ModuleBase {
public:
    MyModule() : api(nullptr), env(nullptr), enable_hack(false), game_data_dir(nullptr), arm_so_data(nullptr), arm_so_length(0) {
        LOGI("MyModule instance created.");
    }

    ~MyModule() {
        LOGI("MyModule instance destroyed.");
        if (game_data_dir) {
            delete[] game_data_dir;
            game_data_dir = nullptr;
        }
        if (arm_so_data && arm_so_length > 0) {
            LOGD("MyModule destructor: munmap'ing arm_so_data that was not consumed.");
            munmap(arm_so_data, arm_so_length);
            arm_so_data = nullptr;
            arm_so_length = 0;
        }
    }

    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        LOGI("MyModule onLoad completed.");
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        auto current_package_name_jstr = args->nice_name;
        auto current_app_data_dir_jstr = args->app_data_dir;

        const char *current_package_name_utf = nullptr;
        const char *current_app_data_dir_utf = nullptr;

        if (current_package_name_jstr) {
            current_package_name_utf = env->GetStringUTFChars(current_package_name_jstr, nullptr);
        }
        if (current_app_data_dir_jstr) {
            current_app_data_dir_utf = env->GetStringUTFChars(current_app_data_dir_jstr, nullptr);
        }

        if (current_package_name_utf && current_app_data_dir_utf) {
            processPreSpecialize(current_package_name_utf, current_app_data_dir_utf);
        } else {
            LOGE("Failed to get package name or app data directory.");
            enable_hack = false;
        }

        if (current_package_name_utf) {
            env->ReleaseStringUTFChars(current_package_name_jstr, current_package_name_utf);
        }
        if (current_app_data_dir_utf) {
            env->ReleaseStringUTFChars(current_app_data_dir_jstr, current_app_data_dir_utf);
        }
    }

    void postAppSpecialize(const AppSpecializeArgs *) override {
        if (enable_hack && game_data_dir) {
            LOGI("Preparing hack for: %s", game_data_dir);
            std::thread hack_thread(hack_prepare, game_data_dir, arm_so_data, arm_so_length);
            hack_thread.detach();
            arm_so_data = nullptr;
            arm_so_length = 0;
        } else {
            LOGI("Hack not enabled or game_data_dir not set for this process.");
            if (arm_so_data && arm_so_length > 0) {
                LOGD("postAppSpecialize: munmap'ing arm_so_data because hack is not enabled.");
                munmap(arm_so_data, arm_so_length);
                arm_so_data = nullptr;
                arm_so_length = 0;
            }
        }
    }

private:
    Api *api;
    JNIEnv *env;
    bool enable_hack;
    char *game_data_dir;
    void *arm_so_data;
    size_t arm_so_length;

    void processPreSpecialize(const char *current_package_name, const char *current_app_data_dir) {
        LOGI("Zygisk Il2Cpp Dumper checking package: %s", current_package_name);
        
        // Lista de prefijos de paquetes comunes que no son juegos
        static const std::vector<std::string> system_prefixes = {
            "com.google.",
            "com.android.",
            "android.",
            "com.qualcomm.",
            "androidx."
        };
        
        // Verificar si es una aplicación del sistema
        for (const auto& prefix : system_prefixes) {
            if (strncmp(current_package_name, prefix.c_str(), prefix.length()) == 0) {
                LOGI("Skipping system package: %s", current_package_name);
                enable_hack = false;
                return;
            }
        }
        
        // Verificar si existe libil2cpp.so en el directorio de la aplicación
        std::string lib_path = std::string(current_app_data_dir) + "/lib/libil2cpp.so";
        if (access(lib_path.c_str(), F_OK) == -1) {
            LOGI("libil2cpp.so not found in %s, skipping hack", lib_path.c_str());
            enable_hack = false;
            return;
        }
        
        // Si llegamos aquí, activar el hack
        enable_hack = true;
        
        // Gestionar game_data_dir
        if (this->game_data_dir) {
            delete[] this->game_data_dir;
        }
        size_t app_data_dir_len = strlen(current_app_data_dir);
        this->game_data_dir = new char[app_data_dir_len + 1];
        strcpy(this->game_data_dir, current_app_data_dir);
        this->game_data_dir[app_data_dir_len] = '\0';
        LOGI("Game data directory set to: %s", this->game_data_dir);

        // Lógica para cargar la librería .so ARM auxiliar en arquitecturas x86/x86_64
#if defined(__i386__) || defined(__x86_64__)
        const char* path = nullptr;
#if defined(__i386__)
        path = "zygisk/armeabi-v7a.so";
        LOGI("Targeting armeabi-v7a helper for x86.");
#elif defined(__x86_64__)
        path = "zygisk/arm64-v8a.so";
        LOGI("Targeting arm64-v8a helper for x86_64.");
#endif
        int dirfd = api->getModuleDir();
        if (dirfd != -1) {
            int fd = openat(dirfd, path, O_RDONLY);
            if (fd != -1) {
                struct stat sb{};
                if (fstat(fd, &sb) == 0) {
                    arm_so_length = sb.st_size;
                    if (arm_so_length > 0) {
                        arm_so_data = mmap(nullptr, arm_so_length, PROT_READ, MAP_PRIVATE, fd, 0);
                        if (arm_so_data == MAP_FAILED) {
                            LOGE("mmap failed for %s: %s", path, strerror(errno));
                            arm_so_data = nullptr;
                            arm_so_length = 0;
                        } else {
                            LOGI("Successfully mmap'd helper library %s, size %zu", path, arm_so_length);
                        }
                    } else {
                        LOGW("Helper library %s is empty.", path);
                        arm_so_data = nullptr;
                        arm_so_length = 0;
                    }
                } else {
                    LOGE("fstat failed for %s: %s", path, strerror(errno));
                    arm_so_data = nullptr;
                    arm_so_length = 0;
                }
                close(fd);
            } else {
                LOGW("Unable to open ARM helper library %s: %s", path, strerror(errno));
                arm_so_data = nullptr;
                arm_so_length = 0;
            }
        } else {
            LOGE("Failed to get module directory descriptor.");
            arm_so_data = nullptr;
            arm_so_length = 0;
        }
#else
        LOGI("Running on ARM architecture. No helper .so mmap needed in main module.");
        arm_so_data = nullptr;
        arm_so_length = 0;
#endif
    }
};

REGISTER_ZYGISK_MODULE(MyModule)