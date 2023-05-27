#include <scwx/qt/manager/timeline_manager.hpp>
#include <scwx/qt/manager/radar_product_manager.hpp>
#include <scwx/util/logger.hpp>
#include <scwx/util/map.hpp>
#include <scwx/util/threads.hpp>
#include <scwx/util/time.hpp>

#include <mutex>

#include <boost/asio/steady_timer.hpp>
#include <fmt/chrono.h>

namespace scwx
{
namespace qt
{
namespace manager
{

static const std::string logPrefix_ = "scwx::qt::manager::timeline_manager";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

enum class Direction
{
   Back,
   Next
};

class TimelineManager::Impl
{
public:
   explicit Impl(TimelineManager* self) : self_ {self} {}

   ~Impl()
   {
      // Lock mutexes before destroying
      std::unique_lock animationTimerLock {animationTimerMutex_};
      animationTimer_.cancel();

      std::unique_lock selectTimeLock {selectTimeMutex_};
   }

   TimelineManager* self_;

   void Pause();
   void Play();
   void SelectTime(std::chrono::system_clock::time_point selectedTime = {});
   void Step(Direction direction);

   std::string                           radarSite_ {"?"};
   std::string                           previousRadarSite_ {"?"};
   std::chrono::system_clock::time_point pinnedTime_ {};
   std::chrono::system_clock::time_point adjustedTime_ {};
   std::chrono::system_clock::time_point selectedTime_ {};
   types::MapTime                        viewType_ {types::MapTime::Live};
   std::chrono::minutes                  loopTime_ {30};
   double                                loopSpeed_ {1.0};

   types::AnimationState     animationState_ {types::AnimationState::Pause};
   boost::asio::steady_timer animationTimer_ {scwx::util::io_context()};
   std::mutex                animationTimerMutex_ {};

   std::mutex selectTimeMutex_ {};
};

TimelineManager::TimelineManager() : p(std::make_unique<Impl>(this)) {}
TimelineManager::~TimelineManager() = default;

void TimelineManager::SetRadarSite(const std::string& radarSite)
{
   if (p->radarSite_ == radarSite)
   {
      // No action needed
      return;
   }

   logger_->debug("SetRadarSite: {}", radarSite);

   p->radarSite_ = radarSite;

   if (p->viewType_ == types::MapTime::Live)
   {
      // If the selected view type is live, select the current products
      p->SelectTime();
   }
   else
   {
      // If the selected view type is archive, select using the selected time
      p->SelectTime(p->selectedTime_);
   }
}

void TimelineManager::SetDateTime(
   std::chrono::system_clock::time_point dateTime)
{
   logger_->debug("SetDateTime: {}", scwx::util::TimeString(dateTime));

   p->pinnedTime_ = dateTime;

   if (p->viewType_ == types::MapTime::Archive)
   {
      // Only select if the view type is archive
      p->SelectTime(dateTime);
   }

   // Ignore a date/time selection if the view type is live
}

void TimelineManager::SetViewType(types::MapTime viewType)
{
   logger_->debug("SetViewType: {}", types::GetMapTimeName(viewType));

   p->viewType_ = viewType;

   if (p->viewType_ == types::MapTime::Live)
   {
      // If the selected view type is live, select the current products
      p->SelectTime();
   }
   else
   {
      // If the selected view type is archive, select using the pinned time
      p->SelectTime(p->pinnedTime_);
   }
}

void TimelineManager::SetLoopTime(std::chrono::minutes loopTime)
{
   logger_->debug("SetLoopTime: {}", loopTime);

   p->loopTime_ = loopTime;
}

void TimelineManager::SetLoopSpeed(double loopSpeed)
{
   logger_->debug("SetLoopSpeed: {}", loopSpeed);

   if (loopSpeed < 1.0)
   {
      loopSpeed = 1.0;
   }

   p->loopSpeed_ = loopSpeed;
}

void TimelineManager::AnimationStepBegin()
{
   logger_->debug("AnimationStepBegin");

   p->Pause();

   if (p->viewType_ == types::MapTime::Live ||
       p->pinnedTime_ == std::chrono::system_clock::time_point {})
   {
      // If the selected view type is live, select the current products
      p->SelectTime(std::chrono::system_clock::now() - p->loopTime_);
   }
   else
   {
      // If the selected view type is archive, select using the pinned time
      p->SelectTime(p->pinnedTime_ - p->loopTime_);
   }
}

void TimelineManager::AnimationStepBack()
{
   logger_->debug("AnimationStepBack");

   p->Pause();
   p->Step(Direction::Back);
}

void TimelineManager::AnimationPlayPause()
{
   if (p->animationState_ == types::AnimationState::Pause)
   {
      logger_->debug("AnimationPlay");
      p->Play();
   }
   else
   {
      logger_->debug("AnimationPause");
      p->Pause();
   }
}

void TimelineManager::AnimationStepNext()
{
   logger_->debug("AnimationStepNext");

   p->Pause();
   p->Step(Direction::Next);
}

void TimelineManager::AnimationStepEnd()
{
   logger_->debug("AnimationStepEnd");

   p->Pause();

   if (p->viewType_ == types::MapTime::Live)
   {
      // If the selected view type is live, select the current products
      p->SelectTime();
   }
   else
   {
      // If the selected view type is archive, select using the pinned time
      p->SelectTime(p->pinnedTime_);
   }
}

void TimelineManager::Impl::Pause()
{
   // Cancel animation
   std::unique_lock animationTimerLock {animationTimerMutex_};
   animationTimer_.cancel();

   if (animationState_ != types::AnimationState::Pause)
   {
      animationState_ = types::AnimationState::Pause;
      emit self_->AnimationStateUpdated(animationState_);
   }
}

void TimelineManager::Impl::Play()
{
   using namespace std::chrono_literals;

   if (animationState_ != types::AnimationState::Play)
   {
      animationState_ = types::AnimationState::Play;
      emit self_->AnimationStateUpdated(animationState_);
   }

   {
      std::unique_lock animationTimerLock {animationTimerMutex_};
      animationTimer_.cancel();
   }

   scwx::util::async(
      [this]()
      {
         // Take a lock for time selection
         std::unique_lock lock {selectTimeMutex_};

         // Determine loop end time
         std::chrono::system_clock::time_point endTime;

         if (viewType_ == types::MapTime::Live ||
             pinnedTime_ == std::chrono::system_clock::time_point {})
         {
            endTime = std::chrono::floor<std::chrono::minutes>(
               std::chrono::system_clock::now());
         }
         else
         {
            endTime = pinnedTime_;
         }

         // Determine loop start time and current position in the loop
         std::chrono::system_clock::time_point startTime = endTime - loopTime_;
         std::chrono::system_clock::time_point currentTime = selectedTime_;
         std::chrono::system_clock::time_point newTime;

         if (currentTime < startTime || currentTime >= endTime)
         {
            // If the currently selected time is out of the loop, select the
            // start time
            newTime = startTime;
         }
         else
         {
            // If the currently selected time is in the loop, increment
            newTime = currentTime + 1min;
         }

         // Unlock prior to selecting time
         lock.unlock();

         // Select the time
         SelectTime(newTime);

         std::chrono::milliseconds interval;
         if (newTime != endTime)
         {
            // Determine repeat interval (speed of 1.0 is 1 minute per second)
            interval =
               std::chrono::milliseconds(std::lroundl(1000.0 / loopSpeed_));
         }
         else
         {
            // Pause for 2.5 seconds at the end of the loop
            interval = std::chrono::milliseconds(2500);
         }

         std::unique_lock animationTimerLock {animationTimerMutex_};

         animationTimer_.expires_after(interval);
         animationTimer_.async_wait(
            [this](const boost::system::error_code& e)
            {
               if (e == boost::system::errc::success)
               {
                  if (animationState_ == types::AnimationState::Play)
                  {
                     Play();
                  }
               }
               else if (e == boost::asio::error::operation_aborted)
               {
                  logger_->debug("Play timer cancelled");
               }
               else
               {
                  logger_->warn("Play timer error: {}", e.message());
               }
            });
      });
}

void TimelineManager::Impl::SelectTime(
   std::chrono::system_clock::time_point selectedTime)
{
   if (selectedTime_ == selectedTime && radarSite_ == previousRadarSite_)
   {
      // Nothing to do
      return;
   }
   else if (selectedTime == std::chrono::system_clock::time_point {})
   {
      // If a default time point is given, reset to a live view
      selectedTime_ = selectedTime;
      adjustedTime_ = selectedTime;

      logger_->debug("Time updated: Live");

      emit self_->VolumeTimeUpdated(selectedTime);
      emit self_->SelectedTimeUpdated(selectedTime);

      return;
   }

   scwx::util::async(
      [=, this]()
      {
         // Take a lock for time selection
         std::unique_lock lock {selectTimeMutex_};

         // Request active volume times
         auto radarProductManager =
            manager::RadarProductManager::Instance(radarSite_);
         auto volumeTimes =
            radarProductManager->GetActiveVolumeTimes(selectedTime);

         // Find the best match bounded time
         auto elementPtr =
            util::GetBoundedElementPointer(volumeTimes, selectedTime);

         if (elementPtr != nullptr)
         {

            // If the adjusted time changed, or if a new radar site has been
            // selected
            if (adjustedTime_ != *elementPtr ||
                radarSite_ != previousRadarSite_)
            {
               // If the time was found, select it
               adjustedTime_ = *elementPtr;

               logger_->debug("Volume time updated: {}",
                              scwx::util::TimeString(adjustedTime_));

               emit self_->VolumeTimeUpdated(adjustedTime_);
            }
         }
         else
         {
            // No volume time was found
            logger_->info("No volume scan found for {}",
                          scwx::util::TimeString(selectedTime));
         }

         logger_->trace("Selected time updated: {}",
                        scwx::util::TimeString(selectedTime));

         selectedTime_ = selectedTime;
         emit self_->SelectedTimeUpdated(selectedTime);

         previousRadarSite_ = radarSite_;
      });
}

void TimelineManager::Impl::Step(Direction direction)
{
   scwx::util::async(
      [=, this]()
      {
         // Take a lock for time selection
         std::unique_lock lock {selectTimeMutex_};

         // Determine time to get active volume times
         std::chrono::system_clock::time_point queryTime = adjustedTime_;
         if (queryTime == std::chrono::system_clock::time_point {})
         {
            queryTime = std::chrono::system_clock::now();
         }

         // Request active volume times
         auto radarProductManager =
            manager::RadarProductManager::Instance(radarSite_);
         auto volumeTimes =
            radarProductManager->GetActiveVolumeTimes(queryTime);

         if (volumeTimes.empty())
         {
            logger_->debug("No products to step through");
            return;
         }

         std::set<std::chrono::system_clock::time_point>::const_iterator it;

         if (adjustedTime_ == std::chrono::system_clock::time_point {})
         {
            // If the adjusted time is live, get the last element in the set
            it = std::prev(volumeTimes.cend());
         }
         else
         {
            // Get the current element in the set
            it = scwx::util::GetBoundedElementIterator(volumeTimes,
                                                       adjustedTime_);
         }

         if (direction == Direction::Back)
         {
            // Only if we aren't at the beginning of the volume times set
            if (it != volumeTimes.cbegin())
            {
               // Select the previous time
               adjustedTime_ = *(--it);
               selectedTime_ = adjustedTime_;

               logger_->debug("Volume time updated: {}",
                              scwx::util::TimeString(adjustedTime_));

               emit self_->VolumeTimeUpdated(adjustedTime_);
               emit self_->SelectedTimeUpdated(adjustedTime_);
            }
         }
         else
         {
            // Only if we aren't at the end of the volume times set
            if (it != std::prev(volumeTimes.cend()))
            {
               // Select the next time
               adjustedTime_ = *(++it);
               selectedTime_ = adjustedTime_;

               logger_->debug("Volume time updated: {}",
                              scwx::util::TimeString(adjustedTime_));

               emit self_->VolumeTimeUpdated(adjustedTime_);
               emit self_->SelectedTimeUpdated(adjustedTime_);
            }
         }
      });
}

std::shared_ptr<TimelineManager> TimelineManager::Instance()
{
   static std::weak_ptr<TimelineManager> timelineManagerReference_ {};
   static std::mutex                     instanceMutex_ {};

   std::unique_lock lock(instanceMutex_);

   std::shared_ptr<TimelineManager> timelineManager =
      timelineManagerReference_.lock();

   if (timelineManager == nullptr)
   {
      timelineManager           = std::make_shared<TimelineManager>();
      timelineManagerReference_ = timelineManager;
   }

   return timelineManager;
}

} // namespace manager
} // namespace qt
} // namespace scwx
