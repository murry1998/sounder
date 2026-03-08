#include <napi.h>

#if __APPLE__
#import <AVFoundation/AVFoundation.h>
#endif

Napi::Value NapiRequestMicrophoneAccess(const Napi::CallbackInfo& info) {
    auto env = info.Env();
#if __APPLE__
    auto deferred = Napi::Promise::Deferred::New(env);
    auto* deferredPtr = new Napi::Promise::Deferred(std::move(deferred));
    auto promise = deferredPtr->Promise();

    auto dummyFn = Napi::Function::New(env, [](const Napi::CallbackInfo& i) -> Napi::Value {
        return i.Env().Undefined();
    });
    auto tsfn = Napi::ThreadSafeFunction::New(env, dummyFn, "MicAccess", 0, 1);

    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
        tsfn.NonBlockingCall([deferredPtr, granted](Napi::Env callbackEnv, Napi::Function) {
            deferredPtr->Resolve(Napi::Boolean::New(callbackEnv, (bool)granted));
            delete deferredPtr;
        });
        tsfn.Release();
    }];

    return promise;
#else
    return Napi::Boolean::New(env, true);
#endif
}
