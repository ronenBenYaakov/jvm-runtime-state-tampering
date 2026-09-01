#ifndef AGENT_CORE_H
#define AGENT_CORE_H

#include <jni.h>
#include <jvmti.h>
#include <atomic>
#include <unordered_set>

extern JavaVM* g_javaVM;
extern jvmtiEnv* g_jvmti;

extern jclass g_referenceClass;
extern jclass g_queueClass;
extern jfieldID g_queueField;
extern jobject g_dummyQueueGlobalRef;

extern std::atomic<bool> g_running;
extern std::atomic<bool> g_armed;
extern std::atomic<jlong> g_nextTag;

void disable_all_reference_queuing(JNIEnv* env);
void drain_and_inspect_leaks(JNIEnv* env);
void agent_loop();

jint JNICALL tag_reference_callback(
    jlong class_tag,
    jlong size,
    jlong* tag_ptr,
    void* user_data
);

#endif // AGENT_CORE_H