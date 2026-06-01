#include <nan.h>
#include <gpiod.h>
#include <uv.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace {

const uint64_t kNanosecondsPerSecond = 1000000000ULL;
const long kWatchTimeoutNanoseconds = 100 * 1000 * 1000;

enum EdgeMode {
  EDGE_BOTH = 0,
  EDGE_RISING = 1,
  EDGE_FALLING = 2,
};

enum BiasMode {
  BIAS_AS_IS = 0,
  BIAS_FLOATING = 1,
  BIAS_PULL_UP = 2,
  BIAS_PULL_DOWN = 3,
};

enum NotificationKind {
  NOTIFICATION_EVENT = 0,
  NOTIFICATION_ERROR = 1,
};

struct Notification {
  NotificationKind kind;
  int eventType;
  uint64_t timestampNs;
  std::string message;
};

bool getRequiredString(v8::Local<v8::Object> options, const char* key, std::string* value) {
  v8::Local<v8::Context> context = Nan::GetCurrentContext();
  v8::Local<v8::String> property = Nan::New(key).ToLocalChecked();
  v8::MaybeLocal<v8::Value> maybeValue = Nan::Get(options, property);

  if (maybeValue.IsEmpty()) {
    Nan::ThrowTypeError((std::string(key) + " is required").c_str());
    return false;
  }

  v8::Local<v8::Value> localValue = maybeValue.ToLocalChecked();

  if (!localValue->IsString()) {
    Nan::ThrowTypeError((std::string(key) + " must be a string").c_str());
    return false;
  }

  Nan::Utf8String utf8(localValue);
  *value = std::string(*utf8, utf8.length());
  return true;
}

bool getOptionalString(v8::Local<v8::Object> options, const char* key, std::string* value) {
  v8::Local<v8::String> property = Nan::New(key).ToLocalChecked();
  v8::MaybeLocal<v8::Value> maybeValue = Nan::Get(options, property);

  if (maybeValue.IsEmpty()) {
    return true;
  }

  v8::Local<v8::Value> localValue = maybeValue.ToLocalChecked();

  if (localValue->IsUndefined() || localValue->IsNull()) {
    return true;
  }

  if (!localValue->IsString()) {
    Nan::ThrowTypeError((std::string(key) + " must be a string").c_str());
    return false;
  }

  Nan::Utf8String utf8(localValue);
  *value = std::string(*utf8, utf8.length());
  return true;
}

bool getRequiredUint32(v8::Local<v8::Object> options, const char* key, uint32_t* value) {
  v8::Local<v8::String> property = Nan::New(key).ToLocalChecked();
  v8::MaybeLocal<v8::Value> maybeValue = Nan::Get(options, property);

  if (maybeValue.IsEmpty()) {
    Nan::ThrowTypeError((std::string(key) + " is required").c_str());
    return false;
  }

  v8::Local<v8::Value> localValue = maybeValue.ToLocalChecked();

  if (!localValue->IsUint32()) {
    Nan::ThrowTypeError((std::string(key) + " must be an unsigned integer").c_str());
    return false;
  }

  *value = Nan::To<uint32_t>(localValue).FromJust();
  return true;
}

bool getRequiredInt(v8::Local<v8::Object> options, const char* key, int* value) {
  v8::Local<v8::String> property = Nan::New(key).ToLocalChecked();
  v8::MaybeLocal<v8::Value> maybeValue = Nan::Get(options, property);

  if (maybeValue.IsEmpty()) {
    Nan::ThrowTypeError((std::string(key) + " is required").c_str());
    return false;
  }

  v8::Local<v8::Value> localValue = maybeValue.ToLocalChecked();

  if (!localValue->IsInt32()) {
    Nan::ThrowTypeError((std::string(key) + " must be an integer").c_str());
    return false;
  }

  *value = Nan::To<int32_t>(localValue).FromJust();
  return true;
}

std::string buildErrorMessage(const std::string& action, const std::string& device, unsigned int pin) {
  return action + " failed for " + device + " line " + std::to_string(pin);
}

class Watcher : public Nan::ObjectWrap {
 public:
  static NAN_MODULE_INIT(Init) {
    v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
    tpl->SetClassName(Nan::New("Watcher").ToLocalChecked());
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    Nan::SetPrototypeMethod(tpl, "close", Close);

    constructor().Reset(Nan::GetFunction(tpl).ToLocalChecked());
    Nan::Set(target, Nan::New("Watcher").ToLocalChecked(), Nan::GetFunction(tpl).ToLocalChecked());
  }

 private:
  Watcher()
      : stopRequested_(false),
        closed_(false),
        asyncInitialized_(false),
        referenced_(false),
        eventCallback_(nullptr),
        errorCallback_(nullptr),
        chip_(nullptr),
        line_(nullptr),
        pin_(0),
        edgeMode_(EDGE_BOTH),
        biasMode_(BIAS_AS_IS) {
    async_.data = this;
  }

  ~Watcher() override {
    closeInternal(false);
  }

  static Nan::Persistent<v8::Function>& constructor() {
    static Nan::Persistent<v8::Function> myConstructor;
    return myConstructor;
  }

  static NAN_METHOD(New) {
    if (!info.IsConstructCall()) {
      Nan::ThrowTypeError("Watcher must be constructed with new");
      return;
    }

    if (info.Length() < 2 || !info[0]->IsObject() || !info[1]->IsFunction()) {
      Nan::ThrowTypeError("Watcher requires options and onEvent callback");
      return;
    }

    auto* watcher = new Watcher();

    if (!watcher->configure(
            info[0].As<v8::Object>(),
            info[1].As<v8::Function>(),
            info.Length() > 2 && info[2]->IsFunction() ? info[2].As<v8::Function>() : v8::Local<v8::Function>())) {
      delete watcher;
      return;
    }

    watcher->Wrap(info.This());
    watcher->Ref();
    watcher->referenced_ = true;

    try {
      watcher->worker_ = std::thread(&Watcher::watchLoop, watcher);
    } catch (const std::exception& error) {
      watcher->closeInternal(false);
      delete watcher;
      Nan::ThrowError(error.what());
      return;
    }

    info.GetReturnValue().Set(info.This());
  }

  static NAN_METHOD(Close) {
    auto* watcher = Nan::ObjectWrap::Unwrap<Watcher>(info.This());
    watcher->closeInternal(true);
  }

  bool configure(v8::Local<v8::Object> options,
                 v8::Local<v8::Function> onEvent,
                 v8::Local<v8::Function> onError) {
    if (!getRequiredString(options, "device", &device_)) {
      return false;
    }

    if (!getRequiredUint32(options, "pin", &pin_)) {
      return false;
    }

    if (!getRequiredInt(options, "edge", &edgeMode_)) {
      return false;
    }

    if (!getRequiredInt(options, "bias", &biasMode_)) {
      return false;
    }

    if (!getOptionalString(options, "consumer", &consumer_)) {
      return false;
    }

    if (edgeMode_ != EDGE_BOTH && edgeMode_ != EDGE_RISING && edgeMode_ != EDGE_FALLING) {
      Nan::ThrowRangeError("edge must be 0, 1, or 2");
      return false;
    }

    if (biasMode_ != BIAS_AS_IS && biasMode_ != BIAS_FLOATING && biasMode_ != BIAS_PULL_UP && biasMode_ != BIAS_PULL_DOWN) {
      Nan::ThrowRangeError("bias must be between 0 and 3");
      return false;
    }

    eventCallback_ = new Nan::Callback(onEvent);

    if (!onError.IsEmpty()) {
      errorCallback_ = new Nan::Callback(onError);
    }

    chip_ = gpiod_chip_open_lookup(device_.c_str());
    if (!chip_) {
      Nan::ThrowError(Nan::ErrnoException(errno, "gpiod_chip_open_lookup", buildErrorMessage("Opening chip", device_, pin_).c_str()));
      return false;
    }

    line_ = gpiod_chip_get_line(chip_, pin_);
    if (!line_) {
      Nan::ThrowError(Nan::ErrnoException(errno, "gpiod_chip_get_line", buildErrorMessage("Opening line", device_, pin_).c_str()));
      return false;
    }

    if (!requestLine()) {
      return false;
    }

    if (uv_async_init(uv_default_loop(), &async_, asyncCallback) != 0) {
      Nan::ThrowError("Unable to initialize watcher callback bridge");
      return false;
    }

    asyncInitialized_ = true;
    return true;
  }

  bool requestLine() {
    int result = -1;

#if GPIOD_VERSION_MAJOR == 1 && GPIOD_VERSION_MINOR >= 5
    int flags = 0;

    switch (biasMode_) {
      case BIAS_FLOATING:
        flags = GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE;
        break;
      case BIAS_PULL_UP:
        flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
        break;
      case BIAS_PULL_DOWN:
        flags = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN;
        break;
      default:
        flags = 0;
        break;
    }

    if (edgeMode_ == EDGE_RISING) {
      result = flags == 0
                   ? gpiod_line_request_rising_edge_events(line_, consumer_.c_str())
                   : gpiod_line_request_rising_edge_events_flags(line_, consumer_.c_str(), flags);
    } else if (edgeMode_ == EDGE_FALLING) {
      result = flags == 0
                   ? gpiod_line_request_falling_edge_events(line_, consumer_.c_str())
                   : gpiod_line_request_falling_edge_events_flags(line_, consumer_.c_str(), flags);
    } else {
      result = flags == 0
                   ? gpiod_line_request_both_edges_events(line_, consumer_.c_str())
                   : gpiod_line_request_both_edges_events_flags(line_, consumer_.c_str(), flags);
    }
#else
    if (biasMode_ != BIAS_AS_IS) {
      Nan::ThrowError("Bias configuration requires libgpiod 1.5 or newer");
      return false;
    }

    if (edgeMode_ == EDGE_RISING) {
      result = gpiod_line_request_rising_edge_events(line_, consumer_.c_str());
    } else if (edgeMode_ == EDGE_FALLING) {
      result = gpiod_line_request_falling_edge_events(line_, consumer_.c_str());
    } else {
      result = gpiod_line_request_both_edges_events(line_, consumer_.c_str());
    }
#endif

    if (result == -1) {
      Nan::ThrowError(Nan::ErrnoException(errno, "gpiod_line_request_events", buildErrorMessage("Requesting edge events", device_, pin_).c_str()));
      return false;
    }

    return true;
  }

  void watchLoop() {
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = kWatchTimeoutNanoseconds;

    while (!stopRequested_.load()) {
      int waitResult = gpiod_line_event_wait(line_, &timeout);

      if (stopRequested_.load()) {
        break;
      }

      if (waitResult == 0) {
        continue;
      }

      if (waitResult < 0) {
        queueError(buildErrorMessage("Waiting for edge event", device_, pin_));
        break;
      }

      struct gpiod_line_event event;
      if (gpiod_line_event_read(line_, &event) < 0) {
        queueError(buildErrorMessage("Reading edge event", device_, pin_));
        break;
      }

      int eventType = event.event_type == GPIOD_LINE_EVENT_RISING_EDGE ? 1 : 0;
      uint64_t timestampNs = static_cast<uint64_t>(event.ts.tv_sec) * kNanosecondsPerSecond +
                             static_cast<uint64_t>(event.ts.tv_nsec);

      queueEvent(eventType, timestampNs);
    }
  }

  void queueEvent(int eventType, uint64_t timestampNs) {
    if (closed_) {
      return;
    }

    Notification notification;
    notification.kind = NOTIFICATION_EVENT;
    notification.eventType = eventType;
    notification.timestampNs = timestampNs;

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      notifications_.push(notification);
    }

    if (asyncInitialized_) {
      uv_async_send(&async_);
    }
  }

  void queueError(const std::string& message) {
    if (closed_) {
      return;
    }

    Notification notification;
    notification.kind = NOTIFICATION_ERROR;
    notification.eventType = 0;
    notification.timestampNs = 0;
    notification.message = message;

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      notifications_.push(notification);
    }

    if (asyncInitialized_) {
      uv_async_send(&async_);
    }
  }

  void drainNotifications() {
    std::queue<Notification> localQueue;

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      std::swap(localQueue, notifications_);
    }

    if (closed_) {
      return;
    }

    Nan::HandleScope scope;

    while (!localQueue.empty()) {
      const Notification& notification = localQueue.front();

      if (notification.kind == NOTIFICATION_EVENT && eventCallback_) {
        v8::Local<v8::Object> event = Nan::New<v8::Object>();
        v8::Local<v8::BigInt> timestampNs = v8::BigInt::NewFromUnsigned(v8::Isolate::GetCurrent(), notification.timestampNs);

        Nan::Set(event, Nan::New("eventType").ToLocalChecked(), Nan::New(notification.eventType));
        Nan::Set(event, Nan::New("timestampNs").ToLocalChecked(), timestampNs);

        v8::Local<v8::Value> argv[] = {event};
        Nan::TryCatch tryCatch;
        eventCallback_->Call(1, argv);
        if (tryCatch.HasCaught()) {
          Nan::FatalException(tryCatch);
        }
      } else if (notification.kind == NOTIFICATION_ERROR && errorCallback_) {
        v8::Local<v8::Value> argv[] = {Nan::Error(notification.message.c_str())};
        Nan::TryCatch tryCatch;
        errorCallback_->Call(1, argv);
        if (tryCatch.HasCaught()) {
          Nan::FatalException(tryCatch);
        }
      }

      localQueue.pop();
    }
  }

  void closeInternal(bool unrefObject) {
    if (closed_) {
      return;
    }

    closed_ = true;
    stopRequested_.store(true);

    if (worker_.joinable()) {
      worker_.join();
    }

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      std::queue<Notification> empty;
      std::swap(notifications_, empty);
    }

    if (asyncInitialized_) {
      asyncInitialized_ = false;
      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&async_))) {
        uv_close(reinterpret_cast<uv_handle_t*>(&async_), nullptr);
      }
    }

    if (line_) {
      gpiod_line_release(line_);
      line_ = nullptr;
    }

    if (chip_) {
      gpiod_chip_close(chip_);
      chip_ = nullptr;
    }

    if (eventCallback_) {
      delete eventCallback_;
      eventCallback_ = nullptr;
    }

    if (errorCallback_) {
      delete errorCallback_;
      errorCallback_ = nullptr;
    }

    if (unrefObject && referenced_) {
      referenced_ = false;
      Unref();
    }
  }

  static void asyncCallback(uv_async_t* handle) {
    auto* watcher = static_cast<Watcher*>(handle->data);

    if (watcher) {
      watcher->drainNotifications();
    }
  }

  std::atomic<bool> stopRequested_;
  bool closed_;
  bool asyncInitialized_;
  bool referenced_;
  uv_async_t async_;
  std::thread worker_;
  std::mutex queueMutex_;
  std::queue<Notification> notifications_;
  Nan::Callback* eventCallback_;
  Nan::Callback* errorCallback_;
  struct gpiod_chip* chip_;
  struct gpiod_line* line_;
  std::string device_;
  std::string consumer_;
  uint32_t pin_;
  int edgeMode_;
  int biasMode_;
};

NAN_MODULE_INIT(initAddon) {
  Watcher::Init(target);
}

NODE_MODULE(libgpiod_watch, initAddon)

}  // namespace