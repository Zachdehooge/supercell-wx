#include "settings_dialog.hpp"
#include "ui_settings_dialog.h"

#include <scwx/awips/phenomenon.hpp>
#include <scwx/qt/config/radar_site.hpp>
#include <scwx/qt/manager/settings_manager.hpp>
#include <scwx/qt/settings/settings_interface.hpp>
#include <scwx/qt/ui/radar_site_dialog.hpp>
#include <scwx/qt/util/color.hpp>
#include <scwx/util/logger.hpp>

#include <format>

#include <QColorDialog>
#include <QFileDialog>
#include <QToolButton>

namespace scwx
{
namespace qt
{
namespace ui
{

static const std::string logPrefix_ = "scwx::qt::ui::settings_dialog";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

static const std::array<std::pair<std::string, std::string>, 17>
   kColorTableTypes_ {std::pair {"BR", "BR"},
                      std::pair {"BV", "BV"},
                      std::pair {"SW", "SW"},
                      std::pair {"ZDR", "ZDR"},
                      std::pair {"PHI2", "PHI2"},
                      std::pair {"CC", "CC"},
                      std::pair {"DOD", "DOD"},
                      std::pair {"DSD", "DSD"},
                      std::pair {"ET", "ET"},
                      std::pair {"OHP", "OHP"},
                      std::pair {"OHPIN", "OHPIN"},
                      std::pair {"PHI3", "PHI3"},
                      std::pair {"SRV", "SRV"},
                      std::pair {"STP", "STP"},
                      std::pair {"STPIN", "STPIN"},
                      std::pair {"VIL", "VIL"},
                      std::pair {"???", "Default"}};

class SettingsDialogImpl
{
public:
   explicit SettingsDialogImpl(SettingsDialog* self) :
       self_ {self},
       radarSiteDialog_ {new RadarSiteDialog(self)},
       settings_ {std::initializer_list<settings::SettingsInterfaceBase*> {
          &defaultRadarSite_,
          &fontSizes_,
          &gridWidth_,
          &gridHeight_,
          &mapboxApiKey_,
          &debugEnabled_}}
   {
      // Configure default alert phenomena colors
      auto& paletteSettings = manager::SettingsManager::palette_settings();
      int   index           = 0;

      for (auto& phenomenon : settings::PaletteSettings::alert_phenomena())
      {
         QColorDialog::setCustomColor(
            index++,
            QColor(QString::fromStdString(
               paletteSettings.alert_color(phenomenon, true).GetDefault())));
         QColorDialog::setCustomColor(
            index++,
            QColor(QString::fromStdString(
               paletteSettings.alert_color(phenomenon, false).GetDefault())));
      }
   }
   ~SettingsDialogImpl() = default;

   void ConnectSignals();
   void SetupGeneralTab();
   void SetupPalettesColorTablesTab();
   void SetupPalettesAlertsTab();

   void ShowColorDialog(QLineEdit* lineEdit, QFrame* frame = nullptr);
   void UpdateRadarDialogLocation(const std::string& id);

   void ApplyChanges();
   void DiscardChanges();
   void ResetToDefault();

   static std::string
               RadarSiteLabel(std::shared_ptr<config::RadarSite>& radarSite);
   static void SetBackgroundColor(const std::string& value, QFrame* frame);

   SettingsDialog*  self_;
   RadarSiteDialog* radarSiteDialog_;

   settings::SettingsInterface<std::string>               defaultRadarSite_ {};
   settings::SettingsInterface<std::vector<std::int64_t>> fontSizes_ {};
   settings::SettingsInterface<std::int64_t>              gridWidth_ {};
   settings::SettingsInterface<std::int64_t>              gridHeight_ {};
   settings::SettingsInterface<std::string>               mapboxApiKey_ {};
   settings::SettingsInterface<bool>                      debugEnabled_ {};

   std::unordered_map<std::string, settings::SettingsInterface<std::string>>
      colorTables_ {};
   std::unordered_map<awips::Phenomenon,
                      settings::SettingsInterface<std::string>>
      activeAlertColors_ {};
   std::unordered_map<awips::Phenomenon,
                      settings::SettingsInterface<std::string>>
      inactiveAlertColors_ {};

   std::vector<settings::SettingsInterfaceBase*> settings_;
};

SettingsDialog::SettingsDialog(QWidget* parent) :
    QDialog(parent),
    p {std::make_unique<SettingsDialogImpl>(this)},
    ui(new Ui::SettingsDialog)
{
   ui->setupUi(this);

   // General
   p->SetupGeneralTab();

   // Palettes > Color Tables
   p->SetupPalettesColorTablesTab();

   // Palettes > Alerts
   p->SetupPalettesAlertsTab();

   p->ConnectSignals();
}

SettingsDialog::~SettingsDialog()
{
   delete ui;
}

void SettingsDialogImpl::ConnectSignals()
{
   QObject::connect(self_->ui->listWidget,
                    &QListWidget::currentRowChanged,
                    self_->ui->stackedWidget,
                    &QStackedWidget::setCurrentIndex);

   QObject::connect(self_->ui->radarSiteSelectButton,
                    &QAbstractButton::clicked,
                    self_,
                    [this]() { radarSiteDialog_->show(); });

   QObject::connect(radarSiteDialog_,
                    &RadarSiteDialog::accepted,
                    self_,
                    [this]()
                    {
                       std::string id = radarSiteDialog_->radar_site();

                       std::shared_ptr<config::RadarSite> radarSite =
                          config::RadarSite::Get(id);

                       if (radarSite != nullptr)
                       {
                          self_->ui->radarSiteComboBox->setCurrentText(
                             QString::fromStdString(RadarSiteLabel(radarSite)));
                       }
                    });

   // Update the Radar Site dialog "map" location with the currently selected
   // radar site
   auto& defaultRadarSite = *defaultRadarSite_.GetSettingsVariable();
   defaultRadarSite.RegisterValueStagedCallback(
      [this](const std::string& newValue)
      { UpdateRadarDialogLocation(newValue); });

   QObject::connect(
      self_->ui->buttonBox,
      &QDialogButtonBox::clicked,
      self_,
      [this](QAbstractButton* button)
      {
         QDialogButtonBox::ButtonRole role =
            self_->ui->buttonBox->buttonRole(button);

         switch (role)
         {
         case QDialogButtonBox::ButtonRole::AcceptRole: // OK
         case QDialogButtonBox::ButtonRole::ApplyRole:  // Apply
            ApplyChanges();
            break;

         case QDialogButtonBox::ButtonRole::DestructiveRole: // Discard
         case QDialogButtonBox::ButtonRole::RejectRole:      // Cancel
            DiscardChanges();
            break;

         case QDialogButtonBox::ButtonRole::ResetRole: // Restore Defaults
            ResetToDefault();
            break;
         }
      });
}

void SettingsDialogImpl::SetupGeneralTab()
{
   auto radarSites = config::RadarSite::GetAll();

   // Sort radar sites by ID
   std::sort(radarSites.begin(),
             radarSites.end(),
             [](const std::shared_ptr<config::RadarSite>& a,
                const std::shared_ptr<config::RadarSite>& b)
             { return a->id() < b->id(); });

   // Add sorted radar sites
   for (std::shared_ptr<config::RadarSite>& radarSite : radarSites)
   {
      QString text = QString::fromStdString(RadarSiteLabel(radarSite));
      self_->ui->radarSiteComboBox->addItem(text);
   }

   settings::GeneralSettings& generalSettings =
      manager::SettingsManager::general_settings();

   defaultRadarSite_.SetSettingsVariable(generalSettings.default_radar_site());
   defaultRadarSite_.SetMapFromValueFunction(
      [](const std::string& id) -> std::string
      {
         // Get the radar site associated with the ID
         std::shared_ptr<config::RadarSite> radarSite =
            config::RadarSite::Get(id);

         if (radarSite == nullptr)
         {
            // No radar site found, just return the ID
            return id;
         }

         // Add location details to the radar site
         return RadarSiteLabel(radarSite);
      });
   defaultRadarSite_.SetMapToValueFunction(
      [](const std::string& text) -> std::string
      {
         // Find the position of location details
         size_t pos = text.rfind(" (");

         if (pos == std::string::npos)
         {
            // No location details found, just return the text
            return text;
         }

         // Remove location details from the radar site
         return text.substr(0, pos);
      });
   defaultRadarSite_.SetEditWidget(self_->ui->radarSiteComboBox);
   defaultRadarSite_.SetResetButton(self_->ui->resetRadarSiteButton);
   UpdateRadarDialogLocation(generalSettings.default_radar_site().GetValue());

   fontSizes_.SetSettingsVariable(generalSettings.font_sizes());
   fontSizes_.SetEditWidget(self_->ui->fontSizesLineEdit);
   fontSizes_.SetResetButton(self_->ui->resetFontSizesButton);

   gridWidth_.SetSettingsVariable(generalSettings.grid_width());
   gridWidth_.SetEditWidget(self_->ui->gridWidthSpinBox);
   gridWidth_.SetResetButton(self_->ui->resetGridWidthButton);

   gridHeight_.SetSettingsVariable(generalSettings.grid_height());
   gridHeight_.SetEditWidget(self_->ui->gridHeightSpinBox);
   gridHeight_.SetResetButton(self_->ui->resetGridHeightButton);

   mapboxApiKey_.SetSettingsVariable(generalSettings.mapbox_api_key());
   mapboxApiKey_.SetEditWidget(self_->ui->mapboxApiKeyLineEdit);
   mapboxApiKey_.SetResetButton(self_->ui->resetMapboxApiKeyButton);

   debugEnabled_.SetSettingsVariable(generalSettings.debug_enabled());
   debugEnabled_.SetEditWidget(self_->ui->debugEnabledCheckBox);
}

void SettingsDialogImpl::SetupPalettesColorTablesTab()
{
   settings::PaletteSettings& paletteSettings =
      manager::SettingsManager::palette_settings();

   // Palettes > Color Tables
   QGridLayout* colorTableLayout =
      reinterpret_cast<QGridLayout*>(self_->ui->colorTableContents->layout());

   int colorTableRow = 0;
   for (auto& colorTableType : kColorTableTypes_)
   {
      QLineEdit*   lineEdit       = new QLineEdit(self_);
      QToolButton* openFileButton = new QToolButton(self_);
      QToolButton* resetButton    = new QToolButton(self_);

      openFileButton->setText(QObject::tr("..."));

      resetButton->setIcon(
         QIcon {":/res/icons/font-awesome-6/rotate-left-solid.svg"});
      resetButton->setVisible(false);

      colorTableLayout->addWidget(
         new QLabel(colorTableType.second.c_str(), self_), colorTableRow, 0);
      colorTableLayout->addWidget(lineEdit, colorTableRow, 1);
      colorTableLayout->addWidget(openFileButton, colorTableRow, 2);
      colorTableLayout->addWidget(resetButton, colorTableRow, 3);
      ++colorTableRow;

      // Create settings interface
      auto result = colorTables_.emplace(
         colorTableType.first, settings::SettingsInterface<std::string> {});
      auto& pair       = *result.first;
      auto& colorTable = pair.second;

      // Add to settings list
      settings_.push_back(&colorTable);

      colorTable.SetSettingsVariable(
         paletteSettings.palette(colorTableType.first));
      colorTable.SetEditWidget(lineEdit);
      colorTable.SetResetButton(resetButton);

      QObject::connect(
         openFileButton,
         &QAbstractButton::clicked,
         self_,
         [this, lineEdit]()
         {
            static const std::string paletteFilter = "Color Palettes (*.pal)";
            static const std::string allFilter     = "All Files (*)";

            QFileDialog* dialog = new QFileDialog(self_);

            dialog->setFileMode(QFileDialog::ExistingFile);
            dialog->setNameFilters({QObject::tr(paletteFilter.c_str()),
                                    QObject::tr(allFilter.c_str())});
            dialog->setAttribute(Qt::WA_DeleteOnClose);

            QObject::connect(dialog,
                             &QFileDialog::fileSelected,
                             self_,
                             [this, lineEdit](const QString& file)
                             {
                                QString path = QDir::toNativeSeparators(file);

                                logger_->info("Selected palette: {}",
                                              path.toStdString());
                                lineEdit->setText(path);

                                // setText does not emit the textEdited signal
                                emit lineEdit->textEdited(path);
                             });

            dialog->open();
         });
   }
}

void SettingsDialogImpl::SetupPalettesAlertsTab()
{
   settings::PaletteSettings& paletteSettings =
      manager::SettingsManager::palette_settings();

   // Palettes > Alerts
   QGridLayout* alertsLayout =
      reinterpret_cast<QGridLayout*>(self_->ui->alertsFrame->layout());

   QLabel* phenomenonLabel = new QLabel(QObject::tr("Phenomenon"), self_);
   QLabel* activeLabel     = new QLabel(QObject::tr("Active"), self_);
   QLabel* inactiveLabel   = new QLabel(QObject::tr("Inactive"), self_);

   QFont boldFont;
   boldFont.setBold(true);
   phenomenonLabel->setFont(boldFont);
   activeLabel->setFont(boldFont);
   inactiveLabel->setFont(boldFont);

   alertsLayout->addWidget(phenomenonLabel, 0, 0);
   alertsLayout->addWidget(activeLabel, 0, 1, 1, 4);
   alertsLayout->addWidget(inactiveLabel, 0, 5, 1, 4);

   int alertsRow = 1;
   for (auto& phenomenon : settings::PaletteSettings::alert_phenomena())
   {
      QFrame* activeFrame   = new QFrame(self_);
      QFrame* inactiveFrame = new QFrame(self_);

      QLineEdit* activeEdit   = new QLineEdit(self_);
      QLineEdit* inactiveEdit = new QLineEdit(self_);

      QToolButton* activeButton        = new QToolButton(self_);
      QToolButton* inactiveButton      = new QToolButton(self_);
      QToolButton* activeResetButton   = new QToolButton(self_);
      QToolButton* inactiveResetButton = new QToolButton(self_);

      activeFrame->setMinimumHeight(24);
      activeFrame->setMinimumWidth(24);
      activeFrame->setFrameShape(QFrame::Shape::Box);
      activeFrame->setFrameShadow(QFrame::Shadow::Plain);
      inactiveFrame->setMinimumHeight(24);
      inactiveFrame->setMinimumWidth(24);
      inactiveFrame->setFrameShape(QFrame::Shape::Box);
      inactiveFrame->setFrameShadow(QFrame::Shadow::Plain);

      activeButton->setIcon(
         QIcon {":/res/icons/font-awesome-6/palette-solid.svg"});
      inactiveButton->setIcon(
         QIcon {":/res/icons/font-awesome-6/palette-solid.svg"});
      activeResetButton->setIcon(
         QIcon {":/res/icons/font-awesome-6/rotate-left-solid.svg"});
      inactiveResetButton->setIcon(
         QIcon {":/res/icons/font-awesome-6/rotate-left-solid.svg"});

      alertsLayout->addWidget(
         new QLabel(QObject::tr(awips::GetPhenomenonText(phenomenon).c_str()),
                    self_),
         alertsRow,
         0);
      alertsLayout->addWidget(activeFrame, alertsRow, 1);
      alertsLayout->addWidget(activeEdit, alertsRow, 2);
      alertsLayout->addWidget(activeButton, alertsRow, 3);
      alertsLayout->addWidget(activeResetButton, alertsRow, 4);
      alertsLayout->addWidget(inactiveFrame, alertsRow, 5);
      alertsLayout->addWidget(inactiveEdit, alertsRow, 6);
      alertsLayout->addWidget(inactiveButton, alertsRow, 7);
      alertsLayout->addWidget(inactiveResetButton, alertsRow, 8);
      ++alertsRow;

      // Create settings interface
      auto activeResult = activeAlertColors_.emplace(
         phenomenon, settings::SettingsInterface<std::string> {});
      auto inactiveResult = inactiveAlertColors_.emplace(
         phenomenon, settings::SettingsInterface<std::string> {});
      auto& activeColor   = activeResult.first->second;
      auto& inactiveColor = inactiveResult.first->second;

      // Add to settings list
      settings_.push_back(&activeColor);
      settings_.push_back(&inactiveColor);

      auto& activeSetting   = paletteSettings.alert_color(phenomenon, true);
      auto& inactiveSetting = paletteSettings.alert_color(phenomenon, false);

      activeColor.SetSettingsVariable(activeSetting);
      activeColor.SetEditWidget(activeEdit);
      activeColor.SetResetButton(activeResetButton);

      inactiveColor.SetSettingsVariable(inactiveSetting);
      inactiveColor.SetEditWidget(inactiveEdit);
      inactiveColor.SetResetButton(inactiveResetButton);

      SetBackgroundColor(activeSetting.GetValue(), activeFrame);
      SetBackgroundColor(inactiveSetting.GetValue(), inactiveFrame);

      activeSetting.RegisterValueStagedCallback(
         [activeFrame](const std::string& value)
         { SetBackgroundColor(value, activeFrame); });
      inactiveSetting.RegisterValueStagedCallback(
         [inactiveFrame](const std::string& value)
         { SetBackgroundColor(value, inactiveFrame); });

      QObject::connect(activeButton,
                       &QAbstractButton::clicked,
                       self_,
                       [=]() { ShowColorDialog(activeEdit, activeFrame); });
      QObject::connect(inactiveButton,
                       &QAbstractButton::clicked,
                       self_,
                       [=]() { ShowColorDialog(inactiveEdit, inactiveFrame); });
   }
}

void SettingsDialogImpl::ShowColorDialog(QLineEdit* lineEdit, QFrame* frame)
{
   QColorDialog* dialog = new QColorDialog(self_);

   dialog->setAttribute(Qt::WA_DeleteOnClose);
   dialog->setOption(QColorDialog::ColorDialogOption::ShowAlphaChannel);

   QColor initialColor(lineEdit->text());
   if (initialColor.isValid())
   {
      dialog->setCurrentColor(initialColor);
   }

   QObject::connect(
      dialog,
      &QColorDialog::colorSelected,
      self_,
      [this, lineEdit, frame](const QColor& color)
      {
         QString colorName = color.name(QColor::NameFormat::HexArgb);

         logger_->info("Selected color: {}", colorName.toStdString());
         lineEdit->setText(colorName);

         // setText does not emit the textEdited signal
         emit lineEdit->textEdited(colorName);
      });

   dialog->open();
}

void SettingsDialogImpl::SetBackgroundColor(const std::string& value,
                                            QFrame*            frame)
{
   frame->setStyleSheet(
      QString::fromStdString(std::format("background-color: {}", value)));
}

void SettingsDialogImpl::UpdateRadarDialogLocation(const std::string& id)
{
   std::shared_ptr<config::RadarSite> radarSite = config::RadarSite::Get(id);

   if (radarSite != nullptr)
   {
      radarSiteDialog_->HandleMapUpdate(radarSite->latitude(),
                                        radarSite->longitude());
   }
}

void SettingsDialogImpl::ApplyChanges()
{
   logger_->info("Applying settings changes");

   bool committed = false;

   for (auto& setting : settings_)
   {
      committed |= setting->Commit();
   }

   if (committed)
   {
      manager::SettingsManager::SaveSettings();
   }
}

void SettingsDialogImpl::DiscardChanges()
{
   logger_->info("Discarding settings changes");

   for (auto& setting : settings_)
   {
      setting->Reset();
   }
}

void SettingsDialogImpl::ResetToDefault()
{
   logger_->info("Restoring settings to default");

   for (auto& setting : settings_)
   {
      setting->StageDefault();
   }
}

std::string SettingsDialogImpl::RadarSiteLabel(
   std::shared_ptr<config::RadarSite>& radarSite)
{
   return std::format("{} ({})", radarSite->id(), radarSite->location_name());
}

} // namespace ui
} // namespace qt
} // namespace scwx
