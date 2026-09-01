#include "agent_core.h"
#include <thread>

static void JNICALL vm_init(jvmtiEnv* jvmti_env, JNIEnv* env, jthread thread) {
    g_jvmti = jvmti_env;

    // 1. Locate java.lang.ref.Reference
    jclass localRef = env->FindClass("java/lang/ref/Reference");
    if (localRef == nullptr) {
        env->ExceptionClear();
        return;
    }
    g_referenceClass = static_cast<jclass>(env->NewGlobalRef(localRef));
    env->DeleteLocalRef(localRef);

    // 2. Locate Reference.queue field
    g_queueField = env->GetFieldID(g_referenceClass, "queue", "Ljava/lang/ref/ReferenceQueue;");
    if (g_queueField == nullptr) {
        env->ExceptionClear();
        return;
    }

    // 3. Instantiate dummy ReferenceQueue
    jclass localQ = env->FindClass("java/lang/ref/ReferenceQueue");
    if (localQ != nullptr) {
        jmethodID initMethod = env->GetMethodID(localQ, "<init>", "()V");
        if (initMethod != nullptr) {
            jobject localDummyQ = env->NewObject(localQ, initMethod);
            if (localDummyQ != nullptr) {
                g_dummyQueueGlobalRef = env->NewGlobalRef(localDummyQ);
                env->DeleteLocalRef(localDummyQ);
            }
        }
        env->DeleteLocalRef(localQ);
    }

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    std::thread(agent_loop).detach();
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* reserved) {
    g_javaVM = vm;
    if (vm->GetEnv(reinterpret_cast<void**>(&g_jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
        return JNI_ERR;
    }

    jvmtiCapabilities capabilities{};
    capabilities.can_tag_objects = 1;
    if (g_jvmti->AddCapabilities(&capabilities) != JVMTI_ERROR_NONE) {
        return JNI_ERR;
    }

    jvmtiEventCallbacks callbacks{};
    callbacks.VMInit = vm_init;
    if (g_jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks)) != JVMTI_ERROR_NONE) {
        return JNI_ERR;
    }

    if (g_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, nullptr) != JVMTI_ERROR_NONE) {
        return JNI_ERR;
    }

    return JNI_OK;
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM* vm) {
    g_running.store(false, std::memory_order_relaxed);
}