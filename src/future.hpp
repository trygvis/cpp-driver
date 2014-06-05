/*
  Copyright 2014 DataStax

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#ifndef __CASS_FUTURE_HPP_INCLUDED__
#define __CASS_FUTURE_HPP_INCLUDED__

#include <uv.h>
#include <assert.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>

#include "cassandra.h"

namespace cass {

struct Error;

template<class T>
class RefCounted {
  public:
      RefCounted(int inital_count = 1)
      : ref_count_(inital_count) { }

    void retain() { ref_count_++; }

    void release() {
      int new_ref_count = --ref_count_;
      assert(new_ref_count >= 0);
      if(new_ref_count == 0) {
        delete static_cast<T*>(this);
      }
    }

  private:
      std::atomic<int> ref_count_;
};

enum FutureType {
  CASS_FUTURE_TYPE_SESSION,
  CASS_FUTURE_TYPE_REQUEST,
};

class Future : public RefCounted<Future> {
  public:
    struct Result : public RefCounted<Future> {
        virtual ~Result() = default;
    };

    struct Error {
        Error(CassError code,
              const std::string& message)
          : code(code)
          , message(message) { }

        CassError code;
        std::string message;
    };

    class ResultOrError {
      public:
        ResultOrError(CassError code, const std::string& message)
          : result_(nullptr)
          , error_(new Error(code, message)) { }

        ResultOrError(Result* result)
          : result_(result)
          , error_(nullptr) { }

        ~ResultOrError() {
          Result* result = release();
          if(result != nullptr) {
            delete result;
          }
        }

        bool is_error() const {
          return error_ != nullptr;
        }

        const Error* error() const {
          return error_.get();
        }

        Result* release() {
          Result* expected = result_;
          if(result_.compare_exchange_strong(expected, nullptr)) {
            return expected;
          }
          return nullptr;
        }

      private:
        std::atomic<Result*> result_;
        std::unique_ptr<const Error> error_;
    };

    Future(FutureType type)
      : RefCounted(2) // waiting + notifying thread
      , type_(type)
      , is_set_(false) { }

    virtual ~Future() = default;

    FutureType type() const { return type_; }

    bool ready() {
      return wait_for(0);
    }

    virtual void wait() {
      std::unique_lock<std::mutex> lock(mutex_);
      while(!is_set_) {
        cond_.wait(lock);
      }
    }

    virtual bool wait_for(size_t timeout) {
      std::unique_lock<std::mutex> lock(mutex_);
      if(!is_set_) {
        cond_.wait_for(lock, std::chrono::microseconds(timeout));
      }
      return is_set_;
    }

    virtual ResultOrError* get() {
      std::unique_lock<std::mutex> lock(mutex_);
      while(!is_set_) {
        cond_.wait(lock);
      }
      return result_or_error_.get();
    }

    void set_error(CassError code, const std::string& message) {
      std::unique_lock<std::mutex> lock(mutex_);
      result_or_error_.reset(new ResultOrError(code, message));
      is_set_ = true;
      cond_.notify_all();
      lock.unlock();
      release();
    }

    void set_result(Future::Result* result = nullptr) {
      std::unique_lock<std::mutex> lock(mutex_);
      result_or_error_.reset(new ResultOrError(result));
      is_set_ = true;
      cond_.notify_all();
      lock.unlock();
      release();
    }

   private:
     FutureType type_;
     bool is_set_;
     std::mutex mutex_;
     std::condition_variable cond_;
     std::unique_ptr<ResultOrError> result_or_error_;
};

} // namespace cass

#endif