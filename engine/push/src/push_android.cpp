#include <jni.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlib/array.h>
#include <dlib/log.h>
#include <dlib/dstrings.h>
#include <dlib/json.h>
#include <script/script.h>
#include <extension/extension.h>
#include <android_native_app_glue.h>

#define LIB_NAME "push"

extern struct android_app* g_AndroidApp;

struct Push;

#define CMD_REGISTRATION_RESULT (0)
#define CMD_PUSH_MESSAGE_RESULT (1)

struct Command
{
    Command()
    {
        memset(this, 0, sizeof(*this));
    }
    uint32_t m_Command;
    int32_t  m_ResponseCode;
    void*    m_Data1;
    void*    m_Data2;
};

static JNIEnv* Attach()
{
    JNIEnv* env;
    g_AndroidApp->activity->vm->AttachCurrentThread(&env, NULL);
    return env;
}

static void Detach()
{
    g_AndroidApp->activity->vm->DetachCurrentThread();
}

struct PushListener
{
    PushListener()
    {
        m_L = 0;
        m_Callback = LUA_NOREF;
        m_Self = LUA_NOREF;
    }
    lua_State* m_L;
    int        m_Callback;
    int        m_Self;
};

struct Push
{
    Push()
    {
        memset(this, 0, sizeof(*this));
        m_Callback = LUA_NOREF;
        m_Self = LUA_NOREF;
        m_Listener.m_Callback = LUA_NOREF;
        m_Listener.m_Self = LUA_NOREF;
    }
    int                  m_Callback;
    int                  m_Self;
    lua_State*           m_L;
    PushListener         m_Listener;

    jobject              m_Push;
    jobject              m_PushJNI;
    jmethodID            m_Start;
    jmethodID            m_Stop;
    jmethodID            m_Register;
    int                  m_Pipefd[2];
};

Push g_Push;

static void VerifyCallback(lua_State* L)
{
    if (g_Push.m_Callback != LUA_NOREF) {
        dmLogError("Unexpected callback set");
        luaL_unref(L, LUA_REGISTRYINDEX, g_Push.m_Callback);
        luaL_unref(L, LUA_REGISTRYINDEX, g_Push.m_Self);
        g_Push.m_Callback = LUA_NOREF;
        g_Push.m_Self = LUA_NOREF;
        g_Push.m_L = 0;
    }
}

int Push_Register(lua_State* L)
{
    int top = lua_gettop(L);
    VerifyCallback(L);

    // NOTE: We ignore argument one. Only for iOS
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    g_Push.m_Callback = luaL_ref(L, LUA_REGISTRYINDEX);
    dmScript::GetInstance(L);
    g_Push.m_Self = luaL_ref(L, LUA_REGISTRYINDEX);
    g_Push.m_L = L;

    JNIEnv* env = Attach();
    env->CallVoidMethod(g_Push.m_Push, g_Push.m_Register, g_AndroidApp->activity->clazz);
    Detach();

    assert(top == lua_gettop(L));
    return 0;
}

int Push_SetListener(lua_State* L)
{
    Push* push = &g_Push;
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int cb = luaL_ref(L, LUA_REGISTRYINDEX);

    if (push->m_Listener.m_Callback != LUA_NOREF) {
        luaL_unref(push->m_Listener.m_L, LUA_REGISTRYINDEX, push->m_Listener.m_Callback);
        luaL_unref(push->m_Listener.m_L, LUA_REGISTRYINDEX, push->m_Listener.m_Self);
    }

    push->m_Listener.m_L = L;
    push->m_Listener.m_Callback = cb;

    dmScript::GetInstance(L);
    push->m_Listener.m_Self = luaL_ref(L, LUA_REGISTRYINDEX);

    return 0;
}

static const luaL_reg Push_methods[] =
{
    {"register", Push_Register},
    {"set_listener", Push_SetListener},
    {0, 0}
};

// NOTE: Copy-paste from script_json
static int ToLua(lua_State*L, dmJson::Document* doc, const char* json, int index)
{
    const dmJson::Node& n = doc->m_Nodes[index];
    int l = n.m_End - n.m_Start;
    switch (n.m_Type)
    {
    case dmJson::TYPE_PRIMITIVE:
        if (l == 4 && memcmp(json + n.m_Start, "null", 4) == 0) {
            lua_pushnil(L);
        } else if (l == 4 && memcmp(json + n.m_Start, "true", 4) == 0) {
            lua_pushboolean(L, 1);
        } else if (l == 5 && memcmp(json + n.m_Start, "false", 5) == 0) {
            lua_pushboolean(L, 0);
        } else {
            double val = atof(json + n.m_Start);
            lua_pushnumber(L, val);
        }
        return index + 1;

    case dmJson::TYPE_STRING:
        lua_pushlstring(L, json + n.m_Start, l);
        return index + 1;

    case dmJson::TYPE_ARRAY:
        lua_createtable(L, n.m_Size, 0);
        ++index;
        for (int i = 0; i < n.m_Size; ++i) {
            index = ToLua(L, doc, json, index);
            lua_rawseti(L, -2, i+1);
        }
        return index;

    case dmJson::TYPE_OBJECT:
        lua_createtable(L, 0, n.m_Size);
        ++index;
        for (int i = 0; i < n.m_Size; i += 2) {
            index = ToLua(L, doc, json, index);
            index = ToLua(L, doc, json, index);
            lua_rawset(L, -3);
        }

        return index;
    }

    assert(false && "not reached");
    return index;
}

#ifdef __cplusplus
extern "C" {
#endif

static void PushError(lua_State*L, const char* error)
{
    // Could be extended with error codes etc
    if (error != 0) {
        lua_newtable(L);
        lua_pushstring(L, "error");
        lua_pushstring(L, error);
        lua_rawset(L, -3);
    } else {
        lua_pushnil(L);
    }
}

JNIEXPORT void JNICALL Java_com_defold_push_PushJNI_onRegistration(JNIEnv* env, jobject, jstring regId, jstring errorMessage)
{
    const char* ri = env->GetStringUTFChars(regId, 0);
    const char* em = env->GetStringUTFChars(errorMessage, 0);

    Command cmd;
    cmd.m_Command = CMD_REGISTRATION_RESULT;
    if (ri) {
        cmd.m_Data1 = strdup(ri);
    }
    if (em) {
        cmd.m_Data2 = strdup(em);
    }
    if (write(g_Push.m_Pipefd[1], &cmd, sizeof(cmd)) != sizeof(cmd)) {
        dmLogFatal("Failed to write command");
    }
    env->ReleaseStringUTFChars(regId, ri);
    env->ReleaseStringUTFChars(errorMessage, em);
}


JNIEXPORT void JNICALL Java_com_defold_push_PushJNI_onMessage(JNIEnv* env, jobject, jstring json)
{
    const char* j = env->GetStringUTFChars(json, 0);

    Command cmd;
    cmd.m_Command = CMD_PUSH_MESSAGE_RESULT;
    cmd.m_Data1 = strdup(j);
    if (write(g_Push.m_Pipefd[1], &cmd, sizeof(cmd)) != sizeof(cmd)) {
        dmLogFatal("Failed to write command");
    }
    env->ReleaseStringUTFChars(json, j);
}

#ifdef __cplusplus
}
#endif

void HandleRegistrationResult(const Command* cmd)
{
    if (g_Push.m_Callback == LUA_NOREF) {
        dmLogError("No callback set");
        return;
    }

    lua_State* L = g_Push.m_L;
    int top = lua_gettop(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_Push.m_Callback);

    // Setup self
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_Push.m_Self);
    lua_pushvalue(L, -1);
    dmScript::SetInstance(L);

    if (!dmScript::IsInstanceValid(L))
    {
        dmLogError("Could not run push callback because the instance has been deleted.");
        lua_pop(L, 2);
        assert(top == lua_gettop(L));
        return;
    }

    if (cmd->m_Data1) {
        lua_pushstring(L, (const char*) cmd->m_Data1);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        PushError(L, (const char*) cmd->m_Data2);
        dmLogError("GCM error %s", (const char*) cmd->m_Data2);
    }

    int ret = lua_pcall(L, 3, LUA_MULTRET, 0);
    if (ret != 0) {
        dmLogError("Error running push callback: %s", lua_tostring(L,-1));
        lua_pop(L, 1);
    }

    luaL_unref(L, LUA_REGISTRYINDEX, g_Push.m_Callback);
    luaL_unref(L, LUA_REGISTRYINDEX, g_Push.m_Self);
    g_Push.m_Callback = LUA_NOREF;
    g_Push.m_Self = LUA_NOREF;

    assert(top == lua_gettop(L));
}

void HandlePushMessageResult(const Command* cmd)
{
    if (g_Push.m_Listener.m_Callback == LUA_NOREF) {
        dmLogError("No callback set");
        return;
    }

    lua_State* L = g_Push.m_Listener.m_L;
    int top = lua_gettop(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_Push.m_Listener.m_Callback);

    // Setup self
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_Push.m_Listener.m_Self);
    lua_pushvalue(L, -1);
    dmScript::SetInstance(L);

    if (!dmScript::IsInstanceValid(L))
    {
        dmLogError("Could not run push callback because the instance has been deleted.");
        lua_pop(L, 2);
        assert(top == lua_gettop(L));
        return;
    }

    dmJson::Document doc;
    dmJson::Result r = dmJson::Parse((const char*) cmd->m_Data1, &doc);
    if (r == dmJson::RESULT_OK && doc.m_NodeCount > 0) {
        ToLua(L, &doc, (const char*) cmd->m_Data1, 0);
        int ret = lua_pcall(L, 2, LUA_MULTRET, 0);
        if (ret != 0) {
            dmLogError("Error running push callback: %s", lua_tostring(L,-1));
            lua_pop(L, 1);
        }
    } else {
        dmLogError("Failed to parse push response (%d)", r);
    }
    dmJson::Free(&doc);

    assert(top == lua_gettop(L));
}

static int LooperCallback(int fd, int events, void* data)
{
    Push* fb = (Push*)data;
    (void)fb;
    Command cmd;
    if (read(g_Push.m_Pipefd[0], &cmd, sizeof(cmd)) == sizeof(cmd)) {
        switch (cmd.m_Command)
        {
        case CMD_REGISTRATION_RESULT:
            HandleRegistrationResult(&cmd);
            break;
        case CMD_PUSH_MESSAGE_RESULT:
            HandlePushMessageResult(&cmd);
            break;

        default:
            assert(false);
        }

        if (cmd.m_Data1) {
            free(cmd.m_Data1);
        }
        if (cmd.m_Data2) {
            free(cmd.m_Data2);
        }
    }
    else {
        dmLogFatal("read error in looper callback");
    }
    return 1;
}

dmExtension::Result AppInitializePush(dmExtension::AppParams* params)
{
    int result = pipe(g_Push.m_Pipefd);
    if (result != 0) {
        dmLogFatal("Could not open pipe for communication: %d", result);
        return dmExtension::RESULT_INIT_ERROR;
    }

    result = ALooper_addFd(g_AndroidApp->looper, g_Push.m_Pipefd[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, LooperCallback, &g_Push);
    if (result != 1) {
        dmLogFatal("Could not add file descriptor to looper: %d", result);
        close(g_Push.m_Pipefd[0]);
        close(g_Push.m_Pipefd[1]);
        return dmExtension::RESULT_INIT_ERROR;
    }

    JNIEnv* env = Attach();

    jclass activity_class = env->FindClass("android/app/NativeActivity");
    jmethodID get_class_loader = env->GetMethodID(activity_class,"getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject cls = env->CallObjectMethod(g_AndroidApp->activity->clazz, get_class_loader);
    jclass class_loader = env->FindClass("java/lang/ClassLoader");
    jmethodID find_class = env->GetMethodID(class_loader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring str_class_name = env->NewStringUTF("com.defold.push.Push");
    jclass push_class = (jclass)env->CallObjectMethod(cls, find_class, str_class_name);
    env->DeleteLocalRef(str_class_name);

    str_class_name = env->NewStringUTF("com.defold.push.PushJNI");
    jclass push_jni_class = (jclass)env->CallObjectMethod(cls, find_class, str_class_name);
    env->DeleteLocalRef(str_class_name);

    g_Push.m_Start = env->GetMethodID(push_class, "start", "(Landroid/app/Activity;Lcom/defold/push/IPushListener;Ljava/lang/String;)V");
    g_Push.m_Stop = env->GetMethodID(push_class, "stop", "()V");
    g_Push.m_Register = env->GetMethodID(push_class, "register", "(Landroid/app/Activity;)V");

    jmethodID get_instance_method = env->GetStaticMethodID(push_class, "getInstance", "()Lcom/defold/push/Push;");
    g_Push.m_Push = env->NewGlobalRef(env->CallStaticObjectMethod(push_class, get_instance_method));

    jmethodID jni_constructor = env->GetMethodID(push_jni_class, "<init>", "()V");
    g_Push.m_PushJNI = env->NewGlobalRef(env->NewObject(push_jni_class, jni_constructor));

    const char* sender_id = dmConfigFile::GetString(params->m_ConfigFile, "android.gcm_sender_id", "");
    jstring sender_id_string = env->NewStringUTF(sender_id);
    env->CallVoidMethod(g_Push.m_Push, g_Push.m_Start, g_AndroidApp->activity->clazz, g_Push.m_PushJNI, sender_id_string);
    env->DeleteLocalRef(sender_id_string);

    Detach();

    return dmExtension::RESULT_OK;
}

dmExtension::Result AppFinalizePush(dmExtension::AppParams* params)
{
    JNIEnv* env = Attach();
    env->CallVoidMethod(g_Push.m_Push, g_Push.m_Stop);
    env->DeleteGlobalRef(g_Push.m_Push);
    env->DeleteGlobalRef(g_Push.m_PushJNI);
    Detach();
    g_Push.m_Push = NULL;

    int result = ALooper_removeFd(g_AndroidApp->looper, g_Push.m_Pipefd[0]);
    if (result != 1) {
        dmLogFatal("Could not remove fd from looper: %d", result);
    }

    close(g_Push.m_Pipefd[0]);
    close(g_Push.m_Pipefd[1]);

    return dmExtension::RESULT_OK;
}

dmExtension::Result InitializePush(dmExtension::Params* params)
{
    lua_State*L = params->m_L;
    int top = lua_gettop(L);
    luaL_register(L, LIB_NAME, Push_methods);
    lua_pop(L, 1);
    assert(top == lua_gettop(L));
    return dmExtension::RESULT_OK;
}

dmExtension::Result FinalizePush(dmExtension::Params* params)
{
    if (params->m_L == g_Push.m_Listener.m_L && g_Push.m_Listener.m_Callback != LUA_NOREF) {
        luaL_unref(g_Push.m_Listener.m_L, LUA_REGISTRYINDEX, g_Push.m_Listener.m_Callback);
        luaL_unref(g_Push.m_Listener.m_L, LUA_REGISTRYINDEX, g_Push.m_Listener.m_Self);
        g_Push.m_Listener.m_L = 0;
        g_Push.m_Listener.m_Callback = LUA_NOREF;
        g_Push.m_Listener.m_Self = LUA_NOREF;
    }

    return dmExtension::RESULT_OK;
}

DM_DECLARE_EXTENSION(PushExt, "Push", AppInitializePush, AppFinalizePush, InitializePush, FinalizePush)
