// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_LOG_NET_LOG_H_
#define NET_LOG_NET_LOG_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "base/atomicops.h"
#include "base/callback_forward.h"
#include "base/compiler_specific.h"
#include "base/macros.h"
#include "base/observer_list.h"
#include "base/strings/string16.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "net/base/net_export.h"
#include "net/log/net_log_capture_mode.h"
#include "net/log/net_log_event_type.h"
#include "net/log/net_log_source_type.h"

namespace base {
class DictionaryValue;
class Value;
}

namespace net {

// NetLog is the destination for log messages generated by the network stack.
// Each log message has a "source" field which identifies the specific entity
// that generated the message (for example, which URLRequest or which
// SpdySession).
//
// To avoid needing to pass in the "source ID" to the logging functions, NetLog
// is usually accessed through a BoundNetLog, which will always pass in a
// specific source ID.
//
// All methods are thread safe, with the exception that no NetLog or
// NetLog::ThreadSafeObserver functions may be called by an observer's
// OnAddEntry() method.  Doing so will result in a deadlock.
//
// For a broader introduction see the design document:
// https://sites.google.com/a/chromium.org/dev/developers/design-documents/network-stack/netlog
class NET_EXPORT NetLog {
 public:

  // A callback that returns a Value representation of the parameters
  // associated with an event.  If called, it will be called synchronously,
  // so it need not have owning references.  May be called more than once, or
  // not at all.  May return NULL.
  typedef base::Callback<std::unique_ptr<base::Value>(NetLogCaptureMode)>
      ParametersCallback;

  // Identifies the entity that generated this log. The |id| field should
  // uniquely identify the source, and is used by log observers to infer
  // message groupings. Can use NetLog::NextID() to create unique IDs.
  struct NET_EXPORT Source {
    static const uint32_t kInvalidId;

    Source();
    Source(NetLogSourceType type, uint32_t id);
    bool IsValid() const;

    // Adds the source to a DictionaryValue containing event parameters,
    // using the name "source_dependency".
    void AddToEventParameters(base::DictionaryValue* event_params) const;

    // Returns a callback that returns a dictionary with a single entry
    // named "source_dependency" that describes |this|.
    ParametersCallback ToEventParametersCallback() const;

    // Attempts to extract a Source from a set of event parameters.  Returns
    // true and writes the result to |source| on success.  Returns false and
    // makes |source| an invalid source on failure.
    // TODO(mmenke):  Long term, we want to remove this.
    static bool FromEventParameters(base::Value* event_params, Source* source);

    NetLogSourceType type;
    uint32_t id;
  };

  struct NET_EXPORT EntryData {
    EntryData(NetLogEventType type,
              Source source,
              NetLogEventPhase phase,
              base::TimeTicks time,
              const ParametersCallback* parameters_callback);
    ~EntryData();

    const NetLogEventType type;
    const Source source;
    const NetLogEventPhase phase;
    const base::TimeTicks time;
    const ParametersCallback* const parameters_callback;
  };

  // An Entry pre-binds EntryData to a capture mode, so observers will observe
  // the output of ToValue() and ParametersToValue() at their log capture mode
  // rather than the current maximum.
  class NET_EXPORT Entry {
   public:
    Entry(const EntryData* data, NetLogCaptureMode capture_mode);
    ~Entry();

    NetLogEventType type() const { return data_->type; }
    Source source() const { return data_->source; }
    NetLogEventPhase phase() const { return data_->phase; }

    // Serializes the specified event to a Value.  The Value also includes the
    // current time.  Takes in a time to allow back-dating entries.
    std::unique_ptr<base::Value> ToValue() const;

    // Returns the parameters as a Value.  Returns NULL if there are no
    // parameters.
    std::unique_ptr<base::Value> ParametersToValue() const;

   private:
    const EntryData* const data_;

    // Log capture mode when the event occurred.
    const NetLogCaptureMode capture_mode_;

    // It is not safe to copy this class, since |parameters_callback_| may
    // include pointers that become stale immediately after the event is added,
    // even if the code were modified to keep its own copy of the callback.
    DISALLOW_COPY_AND_ASSIGN(Entry);
  };

  // An observer that is notified of entries added to the NetLog. The
  // "ThreadSafe" prefix of the name emphasizes that this observer may be
  // called from different threads then the one which added it as an observer.
  class NET_EXPORT ThreadSafeObserver {
   public:
    // Constructs an observer that wants to see network events, with
    // the specified minimum event granularity.  A ThreadSafeObserver can only
    // observe a single NetLog at a time.
    //
    // Observers will be called on the same thread an entry is added on,
    // and are responsible for ensuring their own thread safety.
    //
    // Observers must stop watching a NetLog before either the observer or the
    // NetLog is destroyed.
    ThreadSafeObserver();

    // Returns the capture mode for events this observer wants to
    // receive. It is only valid to call this while observing a NetLog.
    NetLogCaptureMode capture_mode() const;

    // Returns the NetLog being watched, or nullptr if there is none.
    NetLog* net_log() const;

    // This method is called whenever an entry (event) was added to the NetLog
    // being watched.
    //
    // OnAddEntry() is invoked on the thread which generated the NetLog entry,
    // which may be different from the thread that added this observer.
    //
    // Whenever OnAddEntry() is invoked, the NetLog's mutex is held. The
    // consequences of this are:
    //
    //   * OnAddEntry() will never be called concurrently -- implementations
    //     can rely on this to avoid needing their own synchronization.
    //
    //   * It is illegal for an observer to call back into the NetLog, or the
    //     observer itself, as this can result in deadlock or violating
    //     expectations of non re-entrancy into ThreadSafeObserver.
    virtual void OnAddEntry(const Entry& entry) = 0;

   protected:
    virtual ~ThreadSafeObserver();

   private:
    friend class NetLog;

    void OnAddEntryData(const EntryData& entry_data);

    // Both of these values are only modified by the NetLog.
    NetLogCaptureMode capture_mode_;
    NetLog* net_log_;

    DISALLOW_COPY_AND_ASSIGN(ThreadSafeObserver);
  };

  NetLog();
  virtual ~NetLog();

  // Emits a global event to the log stream, with its own unique source ID.
  void AddGlobalEntry(NetLogEventType type);
  void AddGlobalEntry(NetLogEventType type,
                      const NetLog::ParametersCallback& parameters_callback);

  // Returns a unique ID which can be used as a source ID.  All returned IDs
  // will be unique and greater than 0.
  uint32_t NextID();

  // Returns true if there are any observers attached to the NetLog. This can be
  // used as an optimization to avoid emitting log entries when there is no
  // chance that the data will be consumed.
  bool IsCapturing() const;

  // Adds an observer and sets its log capture mode.  The observer must not be
  // watching any NetLog, including this one, when this is called.
  //
  // DEPRECATED: The ability to watch the netlog stream is being phased out
  // (crbug.com/472693) as it can be misused in production code. Please consult
  // with a net/log OWNER before introducing a new dependency on this.
  void DeprecatedAddObserver(ThreadSafeObserver* observer,
                             NetLogCaptureMode capture_mode);

  // Sets the log capture mode of |observer| to |capture_mode|.  |observer| must
  // be watching |this|.  NetLog implementations must call
  // NetLog::OnSetObserverCaptureMode to update the observer's internal state.
  void SetObserverCaptureMode(ThreadSafeObserver* observer,
                              NetLogCaptureMode capture_mode);

  // Removes an observer.
  //
  // For thread safety reasons, it is recommended that this not be called in
  // an object's destructor.
  //
  // DEPRECATED: The ability to watch the netlog stream is being phased out
  // (crbug.com/472693) as it can be misused in production code. Please consult
  // with a net/log OWNER before introducing a new dependency on this.
  void DeprecatedRemoveObserver(ThreadSafeObserver* observer);

  // Converts a time to the string format that the NetLog uses to represent
  // times.  Strings are used since integers may overflow.
  static std::string TickCountToString(const base::TimeTicks& time);

  // Returns a C-String symbolic name for |event_type|.
  static const char* EventTypeToString(NetLogEventType event_type);

  // Returns a dictionary that maps event type symbolic names to their enum
  // values.  Caller takes ownership of the returned Value.
  static base::Value* GetEventTypesAsValue();

  // Returns a C-String symbolic name for |source_type|.
  static const char* SourceTypeToString(NetLogSourceType source_type);

  // Returns a dictionary that maps source type symbolic names to their enum
  // values.  Caller takes ownership of the returned Value.
  static base::Value* GetSourceTypesAsValue();

  // Returns a C-String symbolic name for |event_phase|.
  static const char* EventPhaseToString(NetLogEventPhase event_phase);

  // Creates a ParametersCallback that encapsulates a single bool.
  // Warning: |name| must remain valid for the life of the callback.
  static ParametersCallback BoolCallback(const char* name, bool value);

  // Warning: |name| must remain valid for the life of the callback.
  static ParametersCallback IntCallback(const char* name, int value);

  // Creates a ParametersCallback that encapsulates a single int64_t.  The
  // callback will return the value as a StringValue, since IntegerValues
  // only support 32-bit values.
  // Warning: |name| must remain valid for the life of the callback.
  static ParametersCallback Int64Callback(const char* name, int64_t value);

  // Creates a ParametersCallback that encapsulates a single UTF8 string.  Takes
  // |value| as a pointer to avoid copying, and emphasize it must be valid for
  // the life of the callback.  |value| may not be NULL.
  // Warning: |name| and |value| must remain valid for the life of the callback.
  static ParametersCallback StringCallback(const char* name,
                                           const std::string* value);
  static ParametersCallback StringCallback(const char* name, const char* value);

  // Same as above, but takes in a UTF16 string.
  static ParametersCallback StringCallback(const char* name,
                                           const base::string16* value);

 private:
  friend class BoundNetLog;

  void AddEntry(NetLogEventType type,
                const Source& source,
                NetLogEventPhase phase,
                const NetLog::ParametersCallback* parameters_callback);

  // Called whenever an observer is added or removed, to update
  // |has_observers_|. Must have acquired |lock_| prior to calling.
  void UpdateIsCapturing();

  // |lock_| protects access to |observers_|.
  base::Lock lock_;

  // Last assigned source ID.  Incremented to get the next one.
  base::subtle::Atomic32 last_id_;

  // |is_capturing_| will be 0 when there are no observers watching the NetLog,
  // 1 otherwise. Note that this is stored as an Atomic32 rather than a boolean
  // so it can be accessed without needing a lock.
  base::subtle::Atomic32 is_capturing_;

  // |lock_| must be acquired whenever reading or writing to this.
  base::ObserverList<ThreadSafeObserver, true> observers_;

  DISALLOW_COPY_AND_ASSIGN(NetLog);
};

// Helper that binds a Source to a NetLog, and exposes convenience methods to
// output log messages without needing to pass in the source.
class NET_EXPORT BoundNetLog {
 public:
  BoundNetLog() : net_log_(NULL) {}
  ~BoundNetLog();

  // Add a log entry to the NetLog for the bound source.
  void AddEntry(NetLogEventType type, NetLogEventPhase phase) const;
  void AddEntry(NetLogEventType type,
                NetLogEventPhase phase,
                const NetLog::ParametersCallback& get_parameters) const;

  // Convenience methods that call AddEntry with a fixed "capture phase"
  // (begin, end, or none).
  void BeginEvent(NetLogEventType type) const;
  void BeginEvent(NetLogEventType type,
                  const NetLog::ParametersCallback& get_parameters) const;

  void EndEvent(NetLogEventType type) const;
  void EndEvent(NetLogEventType type,
                const NetLog::ParametersCallback& get_parameters) const;

  void AddEvent(NetLogEventType type) const;
  void AddEvent(NetLogEventType type,
                const NetLog::ParametersCallback& get_parameters) const;

  // Just like AddEvent, except |net_error| is a net error code.  A parameter
  // called "net_error" with the indicated value will be recorded for the event.
  // |net_error| must be negative, and not ERR_IO_PENDING, as it's not a true
  // error.
  void AddEventWithNetErrorCode(NetLogEventType event_type,
                                int net_error) const;

  // Just like EndEvent, except |net_error| is a net error code.  If it's
  // negative, a parameter called "net_error" with a value of |net_error| is
  // associated with the event.  Otherwise, the end event has no parameters.
  // |net_error| must not be ERR_IO_PENDING, as it's not a true error.
  void EndEventWithNetErrorCode(NetLogEventType event_type,
                                int net_error) const;

  // Logs a byte transfer event to the NetLog.  Determines whether to log the
  // received bytes or not based on the current logging level.
  void AddByteTransferEvent(NetLogEventType event_type,
                            int byte_count,
                            const char* bytes) const;

  bool IsCapturing() const;

  // Helper to create a BoundNetLog given a NetLog and a NetLogSourceType.
  // Takes care of creating a unique source ID, and handles
  //  the case of NULL net_log.
  static BoundNetLog Make(NetLog* net_log, NetLogSourceType source_type);

  const NetLog::Source& source() const { return source_; }
  NetLog* net_log() const { return net_log_; }

 private:
  // TODO(eroman): Temporary until crbug.com/467797 is solved.
  enum Liveness {
    ALIVE = 0xCA11AB13,
    DEAD = 0xDEADBEEF,
  };

  BoundNetLog(const NetLog::Source& source, NetLog* net_log)
      : source_(source), net_log_(net_log) {}

  // TODO(eroman): Temporary until crbug.com/467797 is solved.
  void CrashIfInvalid() const;

  NetLog::Source source_;
  NetLog* net_log_;

  // TODO(eroman): Temporary until crbug.com/467797 is solved.
  Liveness liveness_ = ALIVE;
};

}  // namespace net

#endif  // NET_LOG_NET_LOG_H_