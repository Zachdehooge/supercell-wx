#include <scwx/qt/map/draw_layer.hpp>
#include <scwx/qt/gl/shader_program.hpp>
#include <scwx/util/logger.hpp>

#pragma warning(push, 0)
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#pragma warning(pop)

namespace scwx
{
namespace qt
{
namespace map
{

static const std::string logPrefix_ = "scwx::qt::map::draw_layer";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

class DrawLayerImpl
{
public:
   explicit DrawLayerImpl(std::shared_ptr<MapContext> context) :
       shaderProgram_ {nullptr}, uMVPMatrixLocation_(GL_INVALID_INDEX)
   {
   }

   ~DrawLayerImpl() {}

   std::shared_ptr<gl::ShaderProgram> shaderProgram_;

   GLint uMVPMatrixLocation_;

   std::vector<std::shared_ptr<gl::draw::DrawItem>> drawList_;
};

DrawLayer::DrawLayer(std::shared_ptr<MapContext> context) :
    GenericLayer(context), p(std::make_unique<DrawLayerImpl>(context))
{
}
DrawLayer::~DrawLayer() = default;

void DrawLayer::Initialize()
{
   gl::OpenGLFunctions& gl = context()->gl();

   p->shaderProgram_ =
      context()->GetShaderProgram(":/gl/color.vert", ":/gl/color.frag");

   p->uMVPMatrixLocation_ =
      gl.glGetUniformLocation(p->shaderProgram_->id(), "uMVPMatrix");
   if (p->uMVPMatrixLocation_ == -1)
   {
      logger_->warn("Could not find uMVPMatrix");
   }

   p->shaderProgram_->Use();

   for (auto& item : p->drawList_)
   {
      item->Initialize();
   }
}

void DrawLayer::Render(const QMapbox::CustomLayerRenderParameters& params)
{
   gl::OpenGLFunctions& gl = context()->gl();

   p->shaderProgram_->Use();

   glm::mat4 projection = glm::ortho(0.0f,
                                     static_cast<float>(params.width),
                                     0.0f,
                                     static_cast<float>(params.height));

   gl.glUniformMatrix4fv(
      p->uMVPMatrixLocation_, 1, GL_FALSE, glm::value_ptr(projection));

   for (auto& item : p->drawList_)
   {
      item->Render();
   }
}

void DrawLayer::Deinitialize()
{
   for (auto& item : p->drawList_)
   {
      item->Deinitialize();
   }
}

void DrawLayer::AddDrawItem(std::shared_ptr<gl::draw::DrawItem> drawItem)
{
   p->drawList_.push_back(drawItem);
}

} // namespace map
} // namespace qt
} // namespace scwx
