#include <scwx/qt/map/map_widget.hpp>
#include <scwx/qt/gl/gl.hpp>
#include <scwx/qt/manager/radar_product_manager.hpp>
#include <scwx/qt/manager/settings_manager.hpp>
#include <scwx/qt/map/color_table_layer.hpp>
#include <scwx/qt/map/layer_wrapper.hpp>
#include <scwx/qt/map/overlay_layer.hpp>
#include <scwx/qt/map/radar_product_layer.hpp>
#include <scwx/qt/map/radar_range_layer.hpp>
#include <scwx/qt/view/radar_product_view_factory.hpp>

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QString>

#include <boost/log/trivial.hpp>

namespace scwx
{
namespace qt
{
namespace map
{

static const std::string logPrefix_ = "[scwx::qt::map::map_widget] ";

typedef std::pair<std::string, std::string> MapStyle;

// clang-format off
static const MapStyle streets          { "mapbox://styles/mapbox/streets-v11",           "Streets"};
static const MapStyle outdoors         { "mapbox://styles/mapbox/outdoors-v11",          "Outdoors"};
static const MapStyle light            { "mapbox://styles/mapbox/light-v10",             "Light"};
static const MapStyle dark             { "mapbox://styles/mapbox/dark-v10",              "Dark" };
static const MapStyle satellite        { "mapbox://styles/mapbox/satellite-v9",          "Satellite" };
static const MapStyle satelliteStreets { "mapbox://styles/mapbox/satellite-streets-v11", "Satellite Streets" };
// clang-format on

static const std::array<MapStyle, 6> mapboxStyles_ = {
   {streets, outdoors, light, dark, satellite, satelliteStreets}};

class MapWidgetImpl : public QObject
{
   Q_OBJECT

public:
   explicit MapWidgetImpl(MapWidget*               widget,
                          const QMapboxGLSettings& settings) :
       context_ {std::make_shared<MapContext>()},
       widget_ {widget},
       settings_(settings),
       map_(),
       radarProductManager_ {manager::RadarProductManager::Instance("KLSX")},
       radarProductLayer_ {nullptr},
       overlayLayer_ {nullptr},
       colorTableLayer_ {nullptr},
       lastPos_(),
       currentStyleIndex_ {0},
       frameDraws_(0),
       prevLatitude_ {0.0},
       prevLongitude_ {0.0},
       prevZoom_ {0.0},
       prevBearing_ {0.0},
       prevPitch_ {0.0}
   {
   }
   ~MapWidgetImpl() = default;

   bool UpdateStoredMapParameters();

   std::shared_ptr<MapContext> context_;

   MapWidget*                 widget_;
   QMapboxGLSettings          settings_;
   std::shared_ptr<QMapboxGL> map_;

   std::shared_ptr<manager::RadarProductManager> radarProductManager_;

   std::shared_ptr<common::ColorTable> colorTable_;

   std::shared_ptr<RadarProductLayer> radarProductLayer_;
   std::shared_ptr<OverlayLayer>      overlayLayer_;
   std::shared_ptr<ColorTableLayer>   colorTableLayer_;

   QPointF lastPos_;
   uint8_t currentStyleIndex_;

   uint64_t frameDraws_;

   double prevLatitude_;
   double prevLongitude_;
   double prevZoom_;
   double prevBearing_;
   double prevPitch_;

public slots:
   void Update();
};

MapWidget::MapWidget(const QMapboxGLSettings& settings) :
    p(std::make_unique<MapWidgetImpl>(this, settings))
{
   setFocusPolicy(Qt::StrongFocus);
}

MapWidget::~MapWidget()
{
   // Make sure we have a valid context so we
   // can delete the QMapboxGL.
   makeCurrent();
}

float MapWidget::GetElevation() const
{
   if (p->context_->radarProductView_ != nullptr)
   {
      return p->context_->radarProductView_->elevation();
   }
   else
   {
      return 0.0f;
   }
}

std::vector<float> MapWidget::GetElevationCuts() const
{
   if (p->context_->radarProductView_ != nullptr)
   {
      return p->context_->radarProductView_->GetElevationCuts();
   }
   else
   {
      return {};
   }
}

common::RadarProductGroup MapWidget::GetRadarProductGroup() const
{
   if (p->context_->radarProductView_ != nullptr)
   {
      return p->context_->radarProductView_->GetRadarProductGroup();
   }
   else
   {
      return common::RadarProductGroup::Unknown;
   }
}

std::string MapWidget::GetRadarProductName() const
{

   if (p->context_->radarProductView_ != nullptr)
   {
      return p->context_->radarProductView_->GetRadarProductName();
   }
   else
   {
      return "?";
   }
}

void MapWidget::SelectElevation(float elevation)
{
   if (p->context_->radarProductView_ != nullptr)
   {
      p->context_->radarProductView_->SelectElevation(elevation);
   }
}

void MapWidget::SelectRadarProduct(common::Level2Product product)
{
   float currentElevation = 0.0f;

   std::shared_ptr<view::RadarProductView>& radarProductView =
      p->context_->radarProductView_;

   if (p->context_->radarProductView_ != nullptr)
   {
      currentElevation = p->context_->radarProductView_->elevation();
   }

   radarProductView = view::RadarProductViewFactory::Create(
      product, currentElevation, p->radarProductManager_);

   connect(
      radarProductView.get(),
      &view::RadarProductView::ColorTableUpdated,
      this,
      [&]() { update(); },
      Qt::QueuedConnection);
   connect(
      radarProductView.get(),
      &view::RadarProductView::SweepComputed,
      this,
      [&]()
      {
         RadarRangeLayer::Update(p->map_, radarProductView->range());
         update();
         emit RadarSweepUpdated();
      },
      Qt::QueuedConnection);

   radarProductView->Initialize();

   std::string colorTableFile =
      manager::SettingsManager::palette_settings()->palette(
         common::GetLevel2Palette(product));
   if (!colorTableFile.empty())
   {
      std::shared_ptr<common::ColorTable> colorTable =
         common::ColorTable::Load(colorTableFile);
      radarProductView->LoadColorTable(colorTable);
   }

   if (p->map_ != nullptr)
   {
      AddLayers();
   }
}

void MapWidget::SetActive(bool isActive)
{
   p->context_->settings_.isActive_ = isActive;
   update();
}

void MapWidget::SetMapParameters(
   double latitude, double longitude, double zoom, double bearing, double pitch)
{
   p->map_->setCoordinateZoom({latitude, longitude}, zoom);
   p->map_->setBearing(bearing);
   p->map_->setPitch(pitch);
}

qreal MapWidget::pixelRatio()
{
   return devicePixelRatioF();
}

void MapWidget::changeStyle()
{
   auto& styles = mapboxStyles_;

   p->map_->setStyleUrl(styles[p->currentStyleIndex_].first.c_str());
   setWindowTitle(QString("Mapbox GL: ") +
                  styles[p->currentStyleIndex_].second.c_str());

   if (++p->currentStyleIndex_ == styles.size())
   {
      p->currentStyleIndex_ = 0;
   }
}

void MapWidget::AddLayers()
{
   if (p->context_->radarProductView_ == nullptr)
   {
      return;
   }

   // TODO: Improve this
   if (p->map_->layerExists("radar"))
   {
      p->map_->removeLayer("radar");
   }
   if (p->map_->layerExists("overlay"))
   {
      p->map_->removeLayer("overlay");
   }
   if (p->map_->layerExists("colorTable"))
   {
      p->map_->removeLayer("colorTable");
   }

   p->radarProductLayer_ = std::make_shared<RadarProductLayer>(p->context_);
   p->overlayLayer_      = std::make_shared<OverlayLayer>(p->context_);
   p->colorTableLayer_   = std::make_shared<ColorTableLayer>(p->context_);

   // QMapboxGL::addCustomLayer will take ownership of the std::unique_ptr
   std::unique_ptr<QMapbox::CustomLayerHostInterface> pHost =
      std::make_unique<LayerWrapper>(p->radarProductLayer_);
   std::unique_ptr<QMapbox::CustomLayerHostInterface> pOverlayHost =
      std::make_unique<LayerWrapper>(p->overlayLayer_);
   std::unique_ptr<QMapbox::CustomLayerHostInterface> pColorTableHost =
      std::make_unique<LayerWrapper>(p->colorTableLayer_);

   QString before = "ferry";

   for (const QString& layer : p->map_->layerIds())
   {
      // Draw below tunnels, ferries and roads
      if (layer.startsWith("tunnel") || layer.startsWith("ferry") ||
          layer.startsWith("road"))
      {
         before = layer;
         break;
      }
   }

   p->map_->addCustomLayer("radar", std::move(pHost), before);
   RadarRangeLayer::Add(
      p->map_, p->context_->radarProductView_->range(), before);
   p->map_->addCustomLayer("overlay", std::move(pOverlayHost));
   p->map_->addCustomLayer("colorTable", std::move(pColorTableHost));
}

void MapWidget::keyPressEvent(QKeyEvent* ev)
{
   switch (ev->key())
   {
   case Qt::Key_S: changeStyle(); break;
   case Qt::Key_L:
   {
      for (const QString& layer : p->map_->layerIds())
      {
         qDebug() << "Layer: " << layer;
      }
   }
   break;
   default: break;
   }

   ev->accept();
}

void MapWidget::mousePressEvent(QMouseEvent* ev)
{
   p->lastPos_ = ev->position();

   if (ev->type() == QEvent::MouseButtonPress)
   {
      if (ev->buttons() == (Qt::LeftButton | Qt::RightButton))
      {
         changeStyle();
      }
   }

   if (ev->type() == QEvent::MouseButtonDblClick)
   {
      if (ev->buttons() == Qt::LeftButton)
      {
         p->map_->scaleBy(2.0, p->lastPos_);
      }
      else if (ev->buttons() == Qt::RightButton)
      {
         p->map_->scaleBy(0.5, p->lastPos_);
      }
   }

   ev->accept();
}

void MapWidget::mouseMoveEvent(QMouseEvent* ev)
{
   QPointF delta = ev->position() - p->lastPos_;

   if (!delta.isNull())
   {
      if (ev->buttons() == Qt::LeftButton &&
          ev->modifiers() & Qt::ShiftModifier)
      {
         p->map_->pitchBy(delta.y());
      }
      else if (ev->buttons() == Qt::LeftButton)
      {
         p->map_->moveBy(delta);
      }
      else if (ev->buttons() == Qt::RightButton)
      {
         p->map_->rotateBy(p->lastPos_, ev->position());
      }
   }

   p->lastPos_ = ev->position();
   ev->accept();
}

void MapWidget::wheelEvent(QWheelEvent* ev)
{
   if (ev->angleDelta().y() == 0)
   {
      return;
   }

   float factor = ev->angleDelta().y() / 1200.;
   if (ev->angleDelta().y() < 0)
   {
      factor = factor > -1 ? factor : 1 / factor;
   }

   p->map_->scaleBy(1 + factor, ev->position());

   ev->accept();
}

void MapWidget::initializeGL()
{
   makeCurrent();
   p->context_->gl_.initializeOpenGLFunctions();

   p->map_.reset(new QMapboxGL(nullptr, p->settings_, size(), pixelRatio()));
   connect(p->map_.get(),
           &QMapboxGL::needsRendering,
           p.get(),
           &MapWidgetImpl::Update);

   // Set default location to KLSX.
   p->map_->setCoordinateZoom(QMapbox::Coordinate(38.6986, -90.6828), 9);
   p->UpdateStoredMapParameters();

   QString styleUrl = qgetenv("MAPBOX_STYLE_URL");
   if (styleUrl.isEmpty())
   {
      changeStyle();
   }
   else
   {
      p->map_->setStyleUrl(styleUrl);
      setWindowTitle(QString("Mapbox GL: ") + styleUrl);
   }

   connect(p->map_.get(), &QMapboxGL::mapChanged, this, &MapWidget::mapChanged);
}

void MapWidget::paintGL()
{
   p->frameDraws_++;
   p->map_->resize(size());
   p->map_->setFramebufferObject(defaultFramebufferObject(),
                                 size() * pixelRatio());
   p->map_->render();
}

void MapWidget::mapChanged(QMapboxGL::MapChange mapChange)
{
   switch (mapChange)
   {
   case QMapboxGL::MapChangeDidFinishLoadingStyle: AddLayers(); break;
   }
}

void MapWidgetImpl::Update()
{
   widget_->update();

   if (UpdateStoredMapParameters())
   {
      emit widget_->MapParametersChanged(
         prevLatitude_, prevLongitude_, prevZoom_, prevBearing_, prevPitch_);
   }
}

bool MapWidgetImpl::UpdateStoredMapParameters()
{
   bool changed = false;

   double newLatitude  = map_->latitude();
   double newLongitude = map_->longitude();
   double newZoom      = map_->zoom();
   double newBearing   = map_->bearing();
   double newPitch     = map_->pitch();

   if (prevLatitude_ != newLatitude ||   //
       prevLongitude_ != newLongitude || //
       prevZoom_ != newZoom ||           //
       prevBearing_ != newBearing ||     //
       prevPitch_ != newPitch)
   {
      prevLatitude_  = newLatitude;
      prevLongitude_ = newLongitude;
      prevZoom_      = newZoom;
      prevBearing_   = newBearing;
      prevPitch_     = newPitch;

      changed = true;
   }

   return changed;
}

} // namespace map
} // namespace qt
} // namespace scwx

#include "map_widget.moc"
