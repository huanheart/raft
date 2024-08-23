#include "include/iomanager.hpp"

namespace monsoon {
// 获取事件上下斄1�7
EventContext &FdContext::getEveContext(Event event) {
  switch (event) {
    case READ:
      return read;
    case WRITE:
      return write;
    default:
      CondPanic(false, "getContext error: unknow event");
  }
  throw std::invalid_argument("getContext invalid event");
}
// 重置事件上下斄1�7
void FdContext::resetEveContext(EventContext &ctx) {
  ctx.scheduler = nullptr;
  ctx.fiber.reset();
  ctx.cb = nullptr;
}
// 触发事件（只是将对应的fiber or cb 加入scheduler tasklist＄1�7
void FdContext::triggerEvent(Event event) {
  CondPanic(events & event, "event hasn't been registed");
  events = (Event)(events & ~event);
  EventContext &ctx = getEveContext(event);
  if (ctx.cb) {
    ctx.scheduler->scheduler(ctx.cb);
  } else {
    ctx.scheduler->scheduler(ctx.fiber);
  }
  resetEveContext(ctx);
  return;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name) : Scheduler(threads, use_caller, name) {
  epfd_ = epoll_create(5000);
  int ret = pipe(tickleFds_);
  CondPanic(ret == 0, "pipe error");

  // 注册pipe读句柄的可读事件，用于tickle调度协程
  epoll_event event{};
  memset(&event, 0, sizeof(epoll_event));
  event.events = EPOLLIN | EPOLLET;
  event.data.fd = tickleFds_[0];
  // 边缘触发，设置非阻塞
  ret = fcntl(tickleFds_[0], F_SETFL, O_NONBLOCK);
  CondPanic(ret == 0, "set fd nonblock error");
  // 注册管道读描述符
  ret = epoll_ctl(epfd_, EPOLL_CTL_ADD, tickleFds_[0], &event);
  CondPanic(ret == 0, "epoll_ctl error");

  contextResize(32);

  // 启动scheduler，开始进行协程调庄1�7
  start();
}
IOManager::~IOManager() {
  stop();
  close(epfd_);
  close(tickleFds_[0]);
  close(tickleFds_[1]);

  for (size_t i = 0; i < fdContexts_.size(); i++) {
    if (fdContexts_[i]) {
      delete fdContexts_[i];
    }
  }
}

// 添加事件
int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
  FdContext *fd_ctx = nullptr;
  RWMutex::ReadLock lock(mutex_);
  // TODO：可以使用map代替
  // 找到fd对应的fdCOntext,没有则创廄1�7
  if ((int)fdContexts_.size() > fd) {
    fd_ctx = fdContexts_[fd];
    lock.unlock();
  } else {
    lock.unlock();
    RWMutex::WriteLock lock2(mutex_);
    contextResize(fd * 1.5);
    fd_ctx = fdContexts_[fd];
  }

  // 同一个fd不允许注册重复事仄1�7
  Mutex::Lock ctxLock(fd_ctx->mutex);
  CondPanic(!(fd_ctx->events & event), "addevent error, fd = " + fd);

  int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  epoll_event epevent;
  epevent.events = EPOLLET | fd_ctx->events | event;
  epevent.data.ptr = fd_ctx;

  int ret = epoll_ctl(epfd_, op, fd, &epevent);
  if (ret) {
    std::cout << "addevent: epoll ctl error" << std::endl;
    return -1;
  }
  // 待执行IO事件数量
  ++pendingEventCnt_;

  // 赋��fd对应的event事件的EventContext
  fd_ctx->events = (Event)(fd_ctx->events | event);
  EventContext &event_ctx = fd_ctx->getEveContext(event);
  CondPanic(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb, "event_ctx is nullptr");

  event_ctx.scheduler = Scheduler::GetThis();
  if (cb) {
    // 设置了回调函敄1�7
    event_ctx.cb.swap(cb);
  } else {
    // 未设置回调函数，则将当前协程设置为回调任劄1�7
    event_ctx.fiber = Fiber::GetThis();
    CondPanic(event_ctx.fiber->getState() == Fiber::RUNNING, "state=" + event_ctx.fiber->getState());
  }
  std::cout << "add event success,fd = " << fd << std::endl;
  return 0;
}
// 删除事件 (删除前不会主动触发事仄1�7)
bool IOManager::delEvent(int fd, Event event) {
  RWMutex::ReadLock lock(mutex_);
  if ((int)fdContexts_.size() <= fd) {
    // 找不到当前事件，返回
    return false;
  }
  FdContext *fd_ctx = fdContexts_[fd];
  lock.unlock();

  Mutex::Lock ctxLock(fd_ctx->mutex);
  if (!(fd_ctx->events & event)) {
    return false;
  }
  // 清理指定事件
  Event new_events = (Event)(fd_ctx->events & ~event);
  int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
  epoll_event epevent;
  epevent.events = EPOLLET | new_events;
  epevent.data.ptr = fd_ctx;
  // 注册删除事件
  int ret = epoll_ctl(epfd_, op, fd, &epevent);
  if (ret) {
    std::cout << "delevent: epoll_ctl error" << std::endl;
    return false;
  }
  --pendingEventCnt_;
  fd_ctx->events = new_events;
  EventContext &event_ctx = fd_ctx->getEveContext(event);
  fd_ctx->resetEveContext(event_ctx);
  return true;
}

// 取消事件 （取消前会主动触发事件）
bool IOManager::cancelEvent(int fd, Event event) {
  RWMutex::ReadLock lock(mutex_);
  if ((int)fdContexts_.size() <= fd) {
    // 找不到当前事件，返回
    return false;
  }
  FdContext *fd_ctx = fdContexts_[fd];
  lock.unlock();

  Mutex::Lock ctxLock(fd_ctx->mutex);
  if (!(fd_ctx->events & event)) {
    return false;
  }
  // 清理指定事件
  Event new_events = (Event)(fd_ctx->events & ~event);
  int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
  epoll_event epevent;
  epevent.events = EPOLLET | new_events;
  epevent.data.ptr = fd_ctx;
  // 注册删除事件
  int ret = epoll_ctl(epfd_, op, fd, &epevent);
  if (ret) {
    std::cout << "delevent: epoll_ctl error" << std::endl;
    return false;
  }
  // 删除之前，触发以此事仄1�7
  fd_ctx->triggerEvent(event);
  --pendingEventCnt_;
  return true;
}
// 取消fd扢�有事仄1�7
bool IOManager::cancelAll(int fd) {
  RWMutex::ReadLock lock(mutex_);
  if ((int)fdContexts_.size() <= fd) {
    // 找不到当前事件，返回
    return false;
  }
  FdContext *fd_ctx = fdContexts_[fd];
  lock.unlock();

  Mutex::Lock ctxLock(fd_ctx->mutex);
  if (!fd_ctx->events) {
    return false;
  }

  int op = EPOLL_CTL_DEL;
  epoll_event epevent;
  epevent.events = 0;
  epevent.data.ptr = fd_ctx;
  // 注册删除事件
  int ret = epoll_ctl(epfd_, op, fd, &epevent);
  if (ret) {
    std::cout << "delevent: epoll_ctl error" << std::endl;
    return false;
  }
  // 触发全部已注册事仄1�7
  if (fd_ctx->events & READ) {
    fd_ctx->triggerEvent(READ);
    --pendingEventCnt_;
  }
  if (fd_ctx->events & WRITE) {
    fd_ctx->triggerEvent(WRITE);
    --pendingEventCnt_;
  }
  CondPanic(fd_ctx->events == 0, "fd not totally clear");
  return true;
}
IOManager *IOManager::GetThis() { return dynamic_cast<IOManager *>(Scheduler::GetThis()); }

// 通知调度器有任务到来
void IOManager::tickle() {
  if (!isHasIdleThreads()) {
    // 此时没有空闲的调度线稄1�7
    return;
  }
  // 写pipe管道，使得idle协程凑够epoll_wait逢�出，弢�始调度任劄1�7
  int rt = write(tickleFds_[1], "T", 1);
  CondPanic(rt == 1, "write pipe error");
}

// 调度器无任务则阻塞在idle线程丄1�7
// 当有新事件触发，则���出idle状��，则执行回调函敄1�7
// 当有新的调度任务，则逢�出idle状��，并执行对应任劄1�7
void IOManager::idle() {
  // 以此朢�多检浄1�7256个就绪事仄1�7
  const uint64_t MAX_EVENTS = 256;
  epoll_event *events = new epoll_event[MAX_EVENTS]();
  std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr) { delete[] ptr; });

  while (true) {
    // std::cout << "[IOManager] idle begin..." << std::endl;
    //  获取下一个定时器超时时间，同时判断调度器是否已经stop
    uint64_t next_timeout = 0;
    if (stopping(next_timeout)) {
      std::cout << "name=" << getName() << "idle stopping exit";
      break;
    }

    // 阻塞等待，等待事件发甄1�7 或��1�7 定时器超旄1�7
    int ret = 0;
    do {
      static const int MAX_TIMEOUT = 5000;

      if (next_timeout != ~0ull) {
        next_timeout = std::min((int)next_timeout, MAX_TIMEOUT);
      } else {
        next_timeout = MAX_TIMEOUT;
      }
      // 阻塞等待事件就绪
      ret = epoll_wait(epfd_, events, MAX_EVENTS, (int)next_timeout);
      // std::cout << "wait..." << std::endl;
      if (ret < 0) {
        if (errno == EINTR) {
          // 系统调用被信号中斄1�7
          continue;
        }
        std::cout << "epoll_wait [" << epfd_ << "] errno,err: " << errno << std::endl;
        break;
      } else {
        break;
      }
    } while (true);

    // 收集扢�有超时定时器，执行回调函敄1�7
    std::vector<std::function<void()>> cbs;
    listExpiredCb(cbs);
    if (!cbs.empty()) {
      for (const auto &cb : cbs) {
        scheduler(cb);
      }
      cbs.clear();
    }

    for (int i = 0; i < ret; i++) {
      epoll_event &event = events[i];
      if (event.data.fd == tickleFds_[0]) {
        // pipe管道内数据无意义，只是tickle意义,读完即可
        uint8_t dummy[256];
        // TODO：ET下阻塞读取可能有问题
        while (read(tickleFds_[0], dummy, sizeof(dummy)) > 0)
          ;
        continue;
      }

      //  通过epoll_event的私有指针获取FdContext
      FdContext *fd_ctx = (FdContext *)event.data.ptr;
      Mutex::Lock lock(fd_ctx->mutex);

      // 错误事件 or 挂起事件(对端关闭)
      if (event.events & (EPOLLERR | EPOLLHUP)) {
        std::cout << "error events" << std::endl;
        event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
      }
      // 实际发生的事件类垄1�7
      int real_events = NONE;
      if (event.events & EPOLLIN) {
        real_events |= READ;
      }
      if (event.events & EPOLLOUT) {
        real_events |= WRITE;
      }
      if ((fd_ctx->events & real_events) == NONE) {
        // 触发的事件类型与注册的事件类型无交集
        continue;
      }
      // 剔除已经发生的事件，将剩余的事件重新加入epoll_wait
      // issue: 在处琄1�7 EPOLLERR 戄1�7 EPOLLHUP 事件时，可能霢�要重新注
      // 冄1�7 EPOLLIN 戄1�7 EPOLLOUT 事件，以确保后续的1�7 IO 可以正常进行
      int left_events = (fd_ctx->events & ~real_events);
      int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
      event.events = EPOLLET | left_events;

      int ret2 = epoll_ctl(epfd_, op, fd_ctx->fd, &event);
      if (ret2) {
        std::cout << "epoll_wait [" << epfd_ << "] errno,err: " << errno << std::endl;
        continue;
      }
      // 处理已就绪事仄1�7 （加入scheduler tasklist,未调度执行）
      if (real_events & READ) {
        fd_ctx->triggerEvent(READ);
        --pendingEventCnt_;
      }
      if (real_events & WRITE) {
        fd_ctx->triggerEvent(WRITE);
        --pendingEventCnt_;
      }
    }
    // 处理结束，idle协程yield,此时调度协程可以执行run去tasklist丄1�7
    // 棢�测，拿取新任务去调度
    Fiber::ptr cur = Fiber::GetThis();
    auto raw_ptr = cur.get();
    cur.reset();
    // std::cout << "[IOManager] idle yield..." << std::endl;
    raw_ptr->yield();
  }
}

bool IOManager::stopping() {
  uint64_t timeout = 0;
  return stopping(timeout);
}

bool IOManager::stopping(uint64_t &timeout) {
  // 扢�有待调度的Io事件执行结束后，才允许���凄1�7
  timeout = getNextTimer();
  return timeout == ~0ull && pendingEventCnt_ == 0 && Scheduler::stopping();
}

void IOManager::contextResize(size_t size) {
  fdContexts_.resize(size);
  for (size_t i = 0; i < fdContexts_.size(); i++) {
    if (!fdContexts_[i]) {
      fdContexts_[i] = new FdContext;
      fdContexts_[i]->fd = i;
    }
  }
}
void IOManager::OnTimerInsertedAtFront() { tickle(); }

}  // namespace monsoon
