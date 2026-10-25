#pragma once
// GameAssembly.dll が公開している il2cpp の C API だけでゲーム内部を触る。
// MelonLoader が生成していたプロキシ DLL の代わりに、ここで必要な関数を直接引く。
#include <cstdint>
#include <string>

namespace il2cpp
{
    struct Vector3 { float x, y, z; };

    // 解決した API 群。Init() が true を返したら全部使える。
    bool Init();

    void* Domain();
    void AttachThread();

    // 画像名 (例 "Assembly-CSharp.dll") とクラス名からクラスを探す
    void* FindClass(const char* image, const char* ns, const char* name);
    // 入れ子クラスを名前の一部で探す (コンパイラ生成の <ShowGoatCount>d__404 など、番号が変わっても拾える)
    void* FindNestedClass(void* klass, const char* nameContains);
    // フィールドのオフセット。見つからなければ -1
    int FieldOffset(void* klass, const char* name);
    // 引数の数・非ジェネリックで絞ってメソッドを探す
    const void* FindMethod(void* klass, const char* name, int argc);
    // MethodInfo の先頭はネイティブ関数ポインタ (methodPointer)
    void* MethodPointer(const void* method);

    void* Invoke(const void* method, void* obj, void** args, void** exc);
    void* TypeObject(void* klass);   // System.Type のインスタンス
    void* ResolveICall(const char* name);
    const char* ClassName(void* klass);
    void* ObjectClass(void* obj);

    uint32_t GcHandleNew(void* obj);
    void* GcHandleTarget(uint32_t handle);
    void GcHandleFree(uint32_t handle);

    // ---- オブジェクトの中身を読む小物 ----

    // Il2CppString: klass, monitor, int32 length (+0x10), UTF-16 文字 (+0x14)
    std::wstring ReadString(void* str);
    std::string ToUtf8(const std::wstring& w);

    // Il2CppArray: klass, monitor, bounds, max_length (+0x18), 要素 (+0x20)
    inline size_t ArrayLength(void* arr) { return arr ? *reinterpret_cast<size_t*>((char*)arr + 0x18) : 0; }
    inline void* ArrayAt(void* arr, size_t i) { return reinterpret_cast<void**>((char*)arr + 0x20)[i]; }

    // UnityEngine.Object の最初のフィールド m_CachedPtr。破棄済みなら null になる。
    // 破棄済みのオブジェクトで icall を呼ぶと例外が飛ぶので、必ずこれで確かめる。
    inline bool UnityAlive(void* obj)
    {
        return obj && *reinterpret_cast<void**>((char*)obj + 0x10) != nullptr;
    }

    template <typename T>
    inline T Field(void* obj, int offset) { return *reinterpret_cast<T*>((char*)obj + offset); }
}
