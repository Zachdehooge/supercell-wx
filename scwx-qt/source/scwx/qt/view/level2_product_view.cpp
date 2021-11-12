#include <scwx/qt/view/level2_product_view.hpp>
#include <scwx/common/constants.hpp>

#include <boost/log/trivial.hpp>
#include <boost/range/irange.hpp>
#include <boost/timer/timer.hpp>

namespace scwx
{
namespace qt
{
namespace view
{

static const std::string logPrefix_ = "[scwx::qt::view::level2_product_view] ";

static constexpr uint32_t VERTICES_PER_BIN  = 6;
static constexpr uint32_t VALUES_PER_VERTEX = 2;

static const std::unordered_map<common::Level2Product,
                                wsr88d::rda::DataBlockType>
   blockTypes_ {
      {common::Level2Product::Reflectivity,
       wsr88d::rda::DataBlockType::MomentRef},
      {common::Level2Product::Velocity, wsr88d::rda::DataBlockType::MomentVel},
      {common::Level2Product::SpectrumWidth,
       wsr88d::rda::DataBlockType::MomentSw},
      {common::Level2Product::DifferentialReflectivity,
       wsr88d::rda::DataBlockType::MomentZdr},
      {common::Level2Product::DifferentialPhase,
       wsr88d::rda::DataBlockType::MomentPhi},
      {common::Level2Product::CorrelationCoefficient,
       wsr88d::rda::DataBlockType::MomentRho},
      {common::Level2Product::ClutterFilterPowerRemoved,
       wsr88d::rda::DataBlockType::MomentCfp}};

static std::chrono::system_clock::time_point
TimePoint(uint16_t modifiedJulianDate, uint32_t milliseconds);

class Level2ProductViewImpl
{
public:
   explicit Level2ProductViewImpl(
      common::Level2Product                         product,
      std::shared_ptr<manager::RadarProductManager> radarProductManager) :
       product_ {product},
       radarProductManager_ {radarProductManager},
       latitude_ {},
       longitude_ {},
       sweepTime_ {},
       colorTable_ {},
       colorTableLut_ {}
   {
      auto it = blockTypes_.find(product);

      if (it != blockTypes_.end())
      {
         dataBlockType_ = it->second;
      }
      else
      {
         BOOST_LOG_TRIVIAL(warning) << logPrefix_ << "Unknown product: \""
                                    << common::GetLevel2Name(product) << "\"";
         dataBlockType_ = wsr88d::rda::DataBlockType::Unknown;
      }
   }
   ~Level2ProductViewImpl() = default;

   common::Level2Product                         product_;
   wsr88d::rda::DataBlockType                    dataBlockType_;
   std::shared_ptr<manager::RadarProductManager> radarProductManager_;

   std::shared_ptr<wsr88d::rda::MomentDataBlock> momentDataBlock0_;

   std::vector<float>    vertices_;
   std::vector<uint8_t>  dataMoments8_;
   std::vector<uint16_t> dataMoments16_;

   float latitude_;
   float longitude_;

   std::chrono::system_clock::time_point sweepTime_;

   std::shared_ptr<common::ColorTable>    colorTable_;
   std::vector<boost::gil::rgba8_pixel_t> colorTableLut_;

   std::shared_ptr<common::ColorTable> savedColorTable_;
   float                               savedScale_;
   float                               savedOffset_;
};

Level2ProductView::Level2ProductView(
   common::Level2Product                         product,
   std::shared_ptr<manager::RadarProductManager> radarProductManager) :
    p(std::make_unique<Level2ProductViewImpl>(product, radarProductManager))
{
   connect(radarProductManager.get(),
           &manager::RadarProductManager::Level2DataLoaded,
           this,
           &Level2ProductView::ComputeSweep);
}
Level2ProductView::~Level2ProductView() = default;

const std::vector<boost::gil::rgba8_pixel_t>&
Level2ProductView::color_table() const
{
   if (p->colorTableLut_.size() == 0)
   {
      return RadarProductView::color_table();
   }
   else
   {
      return p->colorTableLut_;
   }
}

std::chrono::system_clock::time_point Level2ProductView::sweep_time() const
{
   return p->sweepTime_;
}

const std::vector<float>& Level2ProductView::vertices() const
{
   return p->vertices_;
}

std::tuple<const void*, size_t, size_t> Level2ProductView::GetMomentData() const
{
   const void* data;
   size_t      dataSize;
   size_t      componentSize;

   if (p->dataMoments8_.size() > 0)
   {
      data          = p->dataMoments8_.data();
      dataSize      = p->dataMoments8_.size() * sizeof(uint8_t);
      componentSize = 1;
   }
   else
   {
      data          = p->dataMoments16_.data();
      dataSize      = p->dataMoments16_.size() * sizeof(uint16_t);
      componentSize = 2;
   }

   return std::tie(data, dataSize, componentSize);
}

void Level2ProductView::LoadColorTable(
   std::shared_ptr<common::ColorTable> colorTable)
{
   p->colorTable_ = colorTable;
   UpdateColorTable();
}

void Level2ProductView::UpdateColorTable()
{
   if (p->momentDataBlock0_ == nullptr || //
       p->colorTable_ == nullptr ||       //
       !p->colorTable_->IsValid())
   {
      // Nothing to update
      return;
   }

   float offset = p->momentDataBlock0_->offset();
   float scale  = p->momentDataBlock0_->scale();

   if (p->savedColorTable_ == p->colorTable_ && //
       p->savedOffset_ == offset &&             //
       p->savedScale_ == scale)
   {
      // The color table LUT does not need updated
      return;
   }

   uint16_t rangeMin;
   uint16_t rangeMax;

   switch (p->product_)
   {
   case common::Level2Product::Reflectivity:
   case common::Level2Product::Velocity:
   case common::Level2Product::SpectrumWidth:
   case common::Level2Product::CorrelationCoefficient:
   default:
      rangeMin = 2;
      rangeMax = 255;
      break;

   case common::Level2Product::DifferentialReflectivity:
      rangeMin = 2;
      rangeMax = 1058;
      break;

   case common::Level2Product::DifferentialPhase:
      rangeMin = 2;
      rangeMax = 1023;
      break;

   case common::Level2Product::ClutterFilterPowerRemoved:
      rangeMin = 8;
      rangeMax = 81;
      break;
   }

   boost::integer_range<uint16_t> dataRange =
      boost::irange<uint16_t>(rangeMin, rangeMax);

   std::vector<boost::gil::rgba8_pixel_t>& lut = p->colorTableLut_;
   lut.resize(rangeMax - rangeMin + 1);

   std::for_each(std::execution::par_unseq,
                 dataRange.begin(),
                 dataRange.end(),
                 [&](uint16_t i) {
                    float f                     = (i - offset) / scale;
                    lut[i - *dataRange.begin()] = p->colorTable_->Color(f);
                 });

   p->savedColorTable_ = p->colorTable_;
   p->savedOffset_     = offset;
   p->savedScale_      = scale;

   emit ColorTableUpdated();
}

void Level2ProductView::ComputeSweep()
{
   BOOST_LOG_TRIVIAL(debug) << logPrefix_ << "ComputeSweep()";

   boost::timer::cpu_timer timer;

   if (p->dataBlockType_ == wsr88d::rda::DataBlockType::Unknown)
   {
      return;
   }

   // TODO: Pick this based on view settings
   auto radarData =
      p->radarProductManager_->GetLevel2Data(p->dataBlockType_, 0);
   if (radarData.size() == 0)
   {
      return;
   }

   const common::RadialSize  radialSize = (radarData.size() == 720) ?
                                             common::RadialSize::_0_5Degree :
                                             common::RadialSize::_1Degree;
   const std::vector<float>& coordinates =
      p->radarProductManager_->coordinates(radialSize);

   auto momentData0     = radarData[0]->moment_data_block(p->dataBlockType_);
   p->momentDataBlock0_ = momentData0;

   if (momentData0 == nullptr)
   {
      BOOST_LOG_TRIVIAL(warning) << logPrefix_ << "No moment data for "
                                 << common::GetLevel2Name(p->product_);
      return;
   }

   auto volumeData0 = radarData[0]->volume_data_block();
   p->latitude_     = volumeData0->latitude();
   p->longitude_    = volumeData0->longitude();
   p->sweepTime_    = TimePoint(radarData[0]->modified_julian_date(),
                             radarData[0]->collection_time());

   // Calculate vertices
   timer.start();

   // Setup vertex vector
   std::vector<float>& vertices = p->vertices_;
   const size_t        radials  = radarData.size();
   const uint32_t      gates    = momentData0->number_of_data_moment_gates();
   size_t              vIndex   = 0;
   vertices.clear();
   vertices.resize(radials * gates * VERTICES_PER_BIN * VALUES_PER_VERTEX);

   // Setup data moment vector
   std::vector<uint8_t>&  dataMoments8  = p->dataMoments8_;
   std::vector<uint16_t>& dataMoments16 = p->dataMoments16_;
   size_t                 mIndex        = 0;

   if (momentData0->data_word_size() == 8)
   {
      dataMoments16.resize(0);
      dataMoments16.shrink_to_fit();

      dataMoments8.resize(radials * gates * VERTICES_PER_BIN);
   }
   else
   {
      dataMoments8.resize(0);
      dataMoments8.shrink_to_fit();

      dataMoments16.resize(radials * gates * VERTICES_PER_BIN);
   }

   // Compute threshold at which to display an individual bin
   const float    scale  = momentData0->scale();
   const float    offset = momentData0->offset();
   const uint16_t snrThreshold =
      std::lroundf(momentData0->snr_threshold_raw() * scale / 10 + offset);

   // Azimuth resolution spacing:
   //   1 = 0.5 degrees
   //   2 = 1.0 degrees
   const float radialMultiplier =
      2.0f /
      std::clamp<int8_t>(radarData[0]->azimuth_resolution_spacing(), 1, 2);

   const float    startAngle  = radarData[0]->azimuth_angle();
   const uint16_t startRadial = std::lroundf(startAngle * radialMultiplier);

   for (uint16_t radial = 0; radial < radials; ++radial)
   {
      auto radialData = radarData[radial];
      auto momentData = radarData[radial]->moment_data_block(p->dataBlockType_);

      if (momentData0->data_word_size() != momentData->data_word_size())
      {
         BOOST_LOG_TRIVIAL(warning)
            << logPrefix_ << "Radial " << radial << " has different word size";
         continue;
      }

      // Compute gate interval
      const uint16_t dataMomentRange = momentData->data_moment_range_raw();
      const uint16_t dataMomentInterval =
         momentData->data_moment_range_sample_interval_raw();
      const uint16_t dataMomentIntervalH = dataMomentInterval / 2;

      // Compute gate size (number of base 250m gates per bin)
      const uint16_t gateSize = std::max<uint16_t>(1, dataMomentInterval / 250);

      // Compute gate range [startGate, endGate)
      const uint16_t startGate = (dataMomentRange - dataMomentIntervalH) / 250;
      const uint16_t numberOfDataMomentGates =
         std::min<uint16_t>(momentData->number_of_data_moment_gates(),
                            static_cast<uint16_t>(gates));
      const uint16_t endGate =
         std::min<uint16_t>(startGate + numberOfDataMomentGates * gateSize,
                            common::MAX_DATA_MOMENT_GATES);

      const uint8_t*  dataMomentsArray8  = nullptr;
      const uint16_t* dataMomentsArray16 = nullptr;

      if (momentData->data_word_size() == 8)
      {
         dataMomentsArray8 =
            reinterpret_cast<const uint8_t*>(momentData->data_moments());
      }
      else
      {
         dataMomentsArray16 =
            reinterpret_cast<const uint16_t*>(momentData->data_moments());
      }

      for (uint16_t gate = startGate, i = 0; gate + gateSize <= endGate;
           gate += gateSize, ++i)
      {
         size_t vertexCount = (gate > 0) ? 6 : 3;

         // Store data moment value
         if (dataMomentsArray8 != nullptr)
         {
            uint8_t dataValue = dataMomentsArray8[i];
            if (dataValue < snrThreshold)
            {
               continue;
            }

            for (size_t m = 0; m < vertexCount; m++)
            {
               dataMoments8[mIndex++] = dataMomentsArray8[i];
            }
         }
         else
         {
            uint16_t dataValue = dataMomentsArray16[i];
            if (dataValue < snrThreshold)
            {
               continue;
            }

            for (size_t m = 0; m < vertexCount; m++)
            {
               dataMoments16[mIndex++] = dataMomentsArray16[i];
            }
         }

         // Store vertices
         if (gate > 0)
         {
            const uint16_t baseCoord = gate - 1;

            size_t offset1 = ((startRadial + radial) % common::MAX_RADIALS *
                                 common::MAX_DATA_MOMENT_GATES +
                              baseCoord) *
                             2;
            size_t offset2 = offset1 + gateSize * 2;
            size_t offset3 =
               (((startRadial + radial + 1) % common::MAX_RADIALS) *
                   common::MAX_DATA_MOMENT_GATES +
                baseCoord) *
               2;
            size_t offset4 = offset3 + gateSize * 2;

            vertices[vIndex++] = coordinates[offset1];
            vertices[vIndex++] = coordinates[offset1 + 1];

            vertices[vIndex++] = coordinates[offset2];
            vertices[vIndex++] = coordinates[offset2 + 1];

            vertices[vIndex++] = coordinates[offset3];
            vertices[vIndex++] = coordinates[offset3 + 1];

            vertices[vIndex++] = coordinates[offset3];
            vertices[vIndex++] = coordinates[offset3 + 1];

            vertices[vIndex++] = coordinates[offset4];
            vertices[vIndex++] = coordinates[offset4 + 1];

            vertices[vIndex++] = coordinates[offset2];
            vertices[vIndex++] = coordinates[offset2 + 1];

            vertexCount = 6;
         }
         else
         {
            const uint16_t baseCoord = gate;

            size_t offset1 = ((startRadial + radial) % common::MAX_RADIALS *
                                 common::MAX_DATA_MOMENT_GATES +
                              baseCoord) *
                             2;
            size_t offset2 =
               (((startRadial + radial + 1) % common::MAX_RADIALS) *
                   common::MAX_DATA_MOMENT_GATES +
                baseCoord) *
               2;

            vertices[vIndex++] = p->latitude_;
            vertices[vIndex++] = p->longitude_;

            vertices[vIndex++] = coordinates[offset1];
            vertices[vIndex++] = coordinates[offset1 + 1];

            vertices[vIndex++] = coordinates[offset2];
            vertices[vIndex++] = coordinates[offset2 + 1];

            vertexCount = 3;
         }
      }
   }
   vertices.resize(vIndex);

   if (momentData0->data_word_size() == 8)
   {
      dataMoments8.resize(mIndex);
   }
   else
   {
      dataMoments16.resize(mIndex);
   }

   timer.stop();
   BOOST_LOG_TRIVIAL(debug)
      << logPrefix_ << "Vertices calculated in " << timer.format(6, "%ws");

   emit SweepComputed();

   UpdateColorTable();
}

std::shared_ptr<Level2ProductView> Level2ProductView::Create(
   common::Level2Product                         product,
   std::shared_ptr<manager::RadarProductManager> radarProductManager)
{
   return std::make_shared<Level2ProductView>(product, radarProductManager);
}

static std::chrono::system_clock::time_point
TimePoint(uint16_t modifiedJulianDate, uint32_t milliseconds)
{
   using namespace std::chrono;
   using sys_days       = time_point<system_clock, days>;
   constexpr auto epoch = sys_days {1969y / December / 31d};

   return epoch + (modifiedJulianDate * 24h) +
          std::chrono::milliseconds {milliseconds};
}

} // namespace view
} // namespace qt
} // namespace scwx