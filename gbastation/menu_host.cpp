#include "menu_host.hpp"
#include "menu_effects.hpp"
#include "session_host.hpp"
#include "session_data.hpp"
#include "TicoCore.h"
#include "TicoUtils.h"
#include "TicoVulkan.h"
#include "woplay/menus/core_menu.hpp"
#include <SDL.h>
#include <algorithm>
#include <ctime>
#include <map>
#include <sstream>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace woplay_menu {
namespace {
using namespace gbastation;
namespace fs = std::filesystem;

class VulkanTextures final : public rendering::RenderBackend {
public:
    rendering::GraphicsApi api() const noexcept override { return rendering::GraphicsApi::Vulkan; }
    const char* name() const noexcept override { return "mGBA Vulkan"; }
    bool initialize(void*, ImGuiContext&) override { return isReady(); }
    bool isReady() const noexcept override { return TicoVulkan::Ctx().device != VK_NULL_HANDLE; }
    bool beginFrame(const rendering::FrameInfo&) override { return false; }
    void renderDrawData(ImDrawData&) override {}
    bool present() override { return false; }
    void waitIdle() noexcept override { TicoVulkan::WaitIdle(); }
    rendering::TextureHandle createTexture(const rendering::PixelView& pixels) override {
        const std::size_t stride = pixels.size.width * 4;
        if (!pixels.pixels || (pixels.rowStride && pixels.rowStride != stride) ||
            pixels.byteCount < stride * pixels.size.height) return {};
        const auto texture = TicoVulkan::CreateTextureRGBA(pixels.pixels, pixels.size.width, pixels.size.height);
        if (texture == ImTextureID_Invalid) return {};
        textures_.emplace(next_, texture);
        return {next_++};
    }
    bool updateTexture(rendering::TextureHandle, const rendering::PixelView&) override { return false; }
    void destroyTexture(rendering::TextureHandle& handle) noexcept override {
        const auto entry = textures_.find(handle.value);
        if (entry != textures_.end()) {
            TicoVulkan::DestroyTexture(entry->second);
            textures_.erase(entry);
        }
        handle = {};
    }
    ImTextureRef textureRef(rendering::TextureHandle handle) const noexcept override {
        const auto entry = textures_.find(handle.value);
        return entry == textures_.end() ? ImTextureRef{} : ImTextureRef(entry->second);
    }
    void shutdown() noexcept override {
        for (const auto& entry : textures_) TicoVulkan::DestroyTexture(entry.second);
        textures_.clear();
    }
private:
    std::map<std::uint64_t, ImTextureID> textures_;
    std::uint64_t next_ = 1;
};

VulkanTextures textures;
resources::Fonts fonts;
rendering::TextureHandle focusGradient, gameIcon, playerAvatar, overlayTexture;
std::string overlayFile;
std::optional<woplay_host::StateRequest> pendingState;
using woplay_host::StateGroup;
using woplay_host::StateOperation;
ui::UiStyle style;
std::unique_ptr<menus::CoreMenu> menu;
std::unique_ptr<VulkanMenuEffects> effects;
TicoCore* running = nullptr;
Action pending = Action::None;
ShaderCallbacks shader;
emulation::Json translations;
bool chinese = true;

struct Preview {
    fs::path path;
    rendering::TextureHandle texture;
    menus::StateSlot entry;
};
std::map<std::pair<StateGroup,int>,Preview> previews;

rendering::TextureHandle upload(const resources::Image& image) {
    if (!image) return {};
    return textures.createTexture({image.rgba.data(), image.rgba.size(), 0,
                                  {static_cast<unsigned>(image.width), static_cast<unsigned>(image.height)}});
}

StateGroup groupFor(menus::SlotGroup group) {
    if(group==menus::SlotGroup::Cycle)return StateGroup::Cycle;
    if(group==menus::SlotGroup::Auto)return StateGroup::Resume;
    return StateGroup::Manual;
}

rendering::TextureHandle picture(const std::string& path) {
    if (path.empty()) return {};
    std::string error;
    return upload(resources::loadImageFile(path,error));
}

menus::StateSlot slotPreview(menus::SlotGroup group, int slot) {
    const auto kind = groupFor(group);
    const auto state = woplay_host::savedStates().entry(kind,slot);
    auto& cache = previews[{kind,slot}];
    const auto name=woplay_host::savedStates().name(kind,slot);
    if (cache.path == state.file) {
        cache.entry.name=name;
        return cache.entry;
    }
    textures.destroyTexture(cache.texture);
    cache.path = state.file;
    cache.entry = {};
    cache.entry.name=name;
    if (!state.file.empty()) {
        cache.entry.occupied = true;
        cache.entry.savedAt = translate("已保存");
        const auto time = static_cast<std::time_t>(state.savedAt);
        std::tm local{};
        char formatted[32]{};
        if (localtime_r(&time,&local) && std::strftime(formatted,sizeof(formatted),"%Y-%m-%d %H:%M:%S",&local))
            cache.entry.savedAt = formatted;
        std::string error;
        const auto image = resources::loadImageFile((state.file.parent_path() / "preview.png").string(),error);
        cache.texture = upload(image);
        if (cache.texture.value)
            cache.entry.image = {textures.textureRef(cache.texture),{0,0},{1,1},{float(image.width),float(image.height)}};
    }
    return cache.entry;
}

void runCommand(menus::CoreCommand command, menus::SlotGroup group, int slot) {
    using C = menus::CoreCommand;
    switch (command) {
    case C::Resume: pending = Action::Resume; break;
    case C::Reset: pending = Action::Reset; break;
    case C::Restart: pending = Action::Restart; break;
    case C::Exit: pending = Action::Exit; break;
    case C::SaveState: pendingState = {StateOperation::Save,groupFor(group),slot}; break;
    case C::LoadState: pendingState = {StateOperation::Load,groupFor(group),slot}; break;
    case C::DeleteState: pendingState = {StateOperation::Delete,groupFor(group),slot}; break;
    case C::ExpandStates: pendingState = {StateOperation::Expand,groupFor(group)}; break;
    case C::Continue: pendingState = {StateOperation::Load,StateGroup::Resume}; break;
    case C::None: break;
    }
}

std::vector<std::pair<std::string,std::string>> overlays() {
    std::vector<std::pair<std::string,std::string>> result{{"未选择",""}};
    for (const auto& file : emulation::listDisplayAssets(woplay_host::module(),woplay_host::directory("root"),"overlays"))
        result.emplace_back(file.filename().string(),file.string());
    return result;
}

std::vector<std::pair<std::string,std::string>> presets() {
    std::vector<std::pair<std::string,std::string>> result{{"关闭", ""}};
    for (const auto& file : emulation::listDisplayAssets(woplay_host::module(),woplay_host::directory("root"),"shaders"))
        result.emplace_back(file.stem().string(),file.string());
    return result;
}
}

std::string translate(const std::string& key) {
    const auto found = translations.find(key);
    if (found != translations.end() && found->is_string()) return found->get<std::string>();
    if (chinese) return key;
    static const std::map<std::string,std::string> common{
        {"暂停菜单","Pause menu"}, {"继续游戏","Resume"}, {"即时存档","Save states"}, {"按键设置","Controls"},
        {"核心设置","Core settings"}, {"游戏设置","Game settings"}, {"退出游戏","Quit game"},
        {"确认","Confirm"}, {"取消","Cancel"}, {"返回","Back"}, {"进入子页","Enter page"}, {"自动存档","Auto save"},
        {"未设置","Unassigned"}, {"当前绑定","Current binding"}, {"替换绑定","Replace"},
        {"添加一组","Add binding"}, {"清除绑定","Clear"}, {"长按取消","Hold to cancel"},
        {"先松开确认键","Release the confirm button first"},
        {"按住按键、组合键或拨动摇杆，松开后保存","Hold the buttons or move a stick. Release to save."},
        {"绑定过长，请替换或清除后重试","Too many inputs. Replace or clear a binding first."},
        {"槽位","Slot"}, {"空槽位","Empty"}, {"无缩略图","No preview"}, {"已保存","Saved"}, {"读取即时存档","Load state"},
        {"只读","Read only"}, {"覆盖存档","Overwrite"},
        {"保存即时存档","Save state"}, {"保存后退出游戏并返回 WoPlay？","Save and return to WoPlay?"},
        {"覆盖选中的即时存档？","Overwrite this save state?"}, {"画面滤镜","Video filters"},
        {"Slang 画面滤镜","Slang video filters"}, {"滤镜预设","Filter preset"},
        {"恢复滤镜参数","Reset filter parameters"}, {"金手指","Cheats"},
        {"此游戏没有金手指文件","No cheat file for this game"}, {"确认操作","Confirm action"},
        {"重置游戏","Reset game"}, {"重新启动","Restart"}, {"重启生效","Restart required"},
        {"继承","Inherited"}, {"恢复继承","Inherit default"}, {"开启","On"}, {"关闭","Off"},
        {"读取上次自动保存的进度？","Load the last automatic save?"},
        {"保存失败，请重试","Save failed. Please retry."},
        {"核心默认","Core defaults"}, {"此游戏","This game"}, {"自动续玩","Resume on launch"},
        {"自动读取","Load automatically"}, {"询问","Ask"},
        {"继续上次游戏","Load auto save"}, {"重新开始","Start from the beginning"},
        {"保存","Save"}, {"读取","Load"}, {"画面","Display"}, {"设置","Settings"}, {"退出","Quit"},
        {"本次游玩","Session time"}, {"循环存档","Rolling saves"}, {"手动存档","Manual saves"},
        {"＋ 扩充档位","+ Add slot"}, {"删除","Delete"}, {"删除选中的即时存档？","Delete this save state?"},
        {"即时存档不可用","Save states unavailable"}, {"没有循环存档","No rolling save"},
        {"读取存档失败","Could not load save"}, {"已读取存档","Save loaded"},
        {"已保存到槽位 %d","Saved to slot %d"}, {"删除存档","Delete save"},
        {"保存当前进度并返回 WoPlay。","Save the current progress and return to WoPlay."},
        {"读取上次退出时保存的进度。","Load the progress saved on exit."},
        {"删除后将无法从此槽位读取。","This slot will become empty."},
        {"当前进度将替换此槽位的存档。","Replace this save with the current progress."},
        {"整数倍缩放倍率 · 0 为自动","Integer scale · 0 is automatic"}, {"自定义参数","Custom values"},
        {"着色器参数","Shader parameters"}, {"自定义画面设置","Custom framing"},
        {"宽度","Width"}, {"高度","Height"}, {"X 坐标","X position"}, {"Y 坐标","Y position"},
        {"快捷存档共用，先填空位，存满覆盖最早一档","Also used by quick save. Fills empty slots, then replaces the oldest."},
        {"与快捷读档共用，只读取最新一档","Also used by quick load. Loads the newest save only."},
        {"存档已删除","Save deleted"}, {"已扩充档位","Slot added"},
        {"尺寸与比例","Size and aspect"}, {"画面模式","Screen mode"}, {"保持比例","Fit"}, {"填充","Fill"}, {"原始","Native"},
        {"整数倍","Integer"}, {"自定义","Custom"}, {"整数倍缩放倍率","Integer scale"}, {"自动","Auto"},
        {"水平偏移","Horizontal offset"}, {"垂直偏移","Vertical offset"},
        {"缩放比例","Scale"}, {"复原","Restore defaults"}, {"个性化设置","Appearance"},
        {"遮罩","Overlay"}, {"着色器","Shader"}, {"启用遮罩","Enable overlay"}, {"选择遮罩图片","Choose overlay"}, {"未选择","None"},
        {"着色器设置","Shader settings"}, {"启用着色器","Enable shader"},
        {"无法读取遮罩图片","Cannot read overlay image"},
        {"重置当前游戏？","Reset the game?"}, {"重新启动以应用核心设置？","Restart to apply core settings?"}
    };
    const auto english = common.find(key);
    return english == common.end() ? key : english->second;
}

bool loadFonts() {
    return resources::Assets(woplay_host::directory("module")+"/ui/assets").loadFonts(fonts);
}

bool initialize(std::function<void()> settingsChanged) {
    try {
        const auto& module = woplay_host::module();
        const auto& saved = woplay_host::theme();
        ui::loadStyleJson(saved.dump(), style);
        const auto needsEffects = [](ui::Material material) {
            return material == ui::Material::Shader;
        };
        for (auto* layer : {static_cast<ui::SurfaceAppearance*>(&style.card), static_cast<ui::SurfaceAppearance*>(&style.tab),
                            static_cast<ui::SurfaceAppearance*>(&style.capsule), static_cast<ui::SurfaceAppearance*>(&style.container),
                            static_cast<ui::SurfaceAppearance*>(&style.button), static_cast<ui::SurfaceAppearance*>(&style.verticalTab),
                            static_cast<ui::SurfaceAppearance*>(&style.toast)}) {
            const auto material = layer->material == ui::Material::Inherit ? style.material : layer->material;
            if (needsEffects(material)) {
                layer->material = layer == &style.container ? ui::Material::Transparent :
                                  layer == &style.card ? ui::Material::Flat : ui::Material::LiquidGlass;
                layer->flatColor.w = std::max(.86f, layer->flatColor.w);
            }
        }
        if (needsEffects(style.material)) style.material = ui::Material::Flat;
        effects = std::make_unique<VulkanMenuEffects>();
        if (!effects->initialize())
            std::fprintf(stderr,"WoPlay Vulkan glass: initialization failed\n");
        // The live game behind the popup has no guaranteed contrast.
        style.container.flatColor.w = std::max(.84f, style.container.flatColor.w);
        auto language = saved.value("locale", std::string("zh-CN"));
        language.resize(std::min<std::size_t>(language.size(), 2));
        chinese = language == "zh";
        translations = emulation::readJson(module.root/"lang"/(language+".json"),
                                           emulation::readJson(module.root/"lang/en.json"));
        const resources::Assets assets((module.root/"ui/assets").string());
        focusGradient = upload(assets.loadImage("frontend/focus_ring_gradient.png"));
        gameIcon = picture(woplay_host::gameIcon());
        playerAvatar = picture(woplay_host::playerAvatar());
        if (!playerAvatar.value) playerAvatar = upload(assets.loadImage("frontend/avatar.png"));
        menus::CoreActions actions;
        actions.resumePolicy = woplay_host::resumePolicy;
        actions.setResumePolicy = [](const std::string& mode) {
            woplay_host::setOptionJson("resume_on_launch", emulation::Json(mode).dump());
        };
        actions.command = runCommand;
        actions.capacity = [](menus::SlotGroup group) {
            try { return woplay_host::savedStates().capacity(groupFor(group)); }
            catch (const std::exception&) { return 0; }
        };
        actions.state = [](menus::SlotGroup group, int slot) {
            try { return slotPreview(group,slot); }
            catch (const std::exception&) { return menus::StateSlot{}; }
        };
        actions.renameState=[](menus::SlotGroup group,int slot,const std::string& name){
            try{woplay_host::renameState(groupFor(group),slot,name);}
            catch(const std::exception& error){notify(error.what());}
        };
#ifdef __SWITCH__
        actions.textInput=[](const std::string& title,const std::string& initial,int limit)->std::optional<std::string>{
            SwkbdConfig keyboard{};
            if(R_FAILED(swkbdCreate(&keyboard,0)))return {};
            swkbdConfigMakePresetDefault(&keyboard);
            swkbdConfigSetHeaderText(&keyboard,title.c_str());
            swkbdConfigSetInitialText(&keyboard,initial.c_str());
            swkbdConfigSetStringLenMax(&keyboard,std::min(limit,500));
            char buffer[2048]{};
            const auto result=swkbdShow(&keyboard,buffer,sizeof(buffer));
            swkbdClose(&keyboard);
            if(R_FAILED(result))return {};
            return std::string(buffer);
        };
#endif
        actions.displaySetting = woplay_host::displaySetting;
        actions.displayPreferences = woplay_host::displayPreferences;
        if (woplay_host::launchingGame()) actions.setDisplayScope = [](bool independent) {
            try { woplay_host::changeDisplayScope(independent); }
            catch (const std::exception& error) { notify(error.what()); }
        };
        actions.selectDisplayPreset = [](const std::string& id) {
            try { woplay_host::chooseDisplayPreset(id); }
            catch (const std::exception& error) { notify(error.what()); }
        };
        actions.setDisplaySetting = [](const std::string& key, const emulation::Json& value) {
            try { woplay_host::changeDisplay(key,value); }
            catch (const std::exception& error) { notify(error.what()); }
        };
        actions.saveDisplayFrame = [](ui::Rect frame, bool keepAspect) {
            try {
                woplay_host::changeDisplay("customFrame",{{"x",frame.min.x},{"y",frame.min.y},
                    {"width",frame.size().x},{"height",frame.size().y},{"keepAspect",keepAspect}});
                return true;
            } catch (const std::exception& error) {
                notify(error.what());
                return false;
            }
        };
        actions.overlays = overlays;
        actions.translate = translate;
        actions.setting = woplay_host::setting;
        actions.overridden = woplay_host::overrides;
        actions.setSetting = [changed = std::move(settingsChanged)](const std::string& key, const emulation::Json& value, bool game) {
            try {
                woplay_host::changeSetting(key, value, game);
                changed();
            } catch (const std::exception& error) { notify(error.what()); }
        };
        actions.cheats = [] {
            std::vector<menus::CheatEntry> entries;
            if (running&&!running->IsHardcoreActive())
                for (const auto& entry : running->GetCheats()){
                    std::string codes;
                    for(const auto& line:entry.codes){if(!codes.empty())codes+='\n';codes+=line;}
                    entries.push_back({entry.name,entry.enabled,std::move(codes),entry.pinned});
                }
            return entries;
        };
        actions.toggleCheat = [](int index) { if (running) running->ToggleCheat(index); };
        actions.cheatFile=[]{
            if(!running||running->IsHardcoreActive())return std::string{};
            const auto file=woplay_host::cheatsFile(".cheats");
            return fs::is_regular_file(fs::symlink_status(file))?file:std::string{};
        };
        actions.openCheatFile=[](const std::string& path){
            try{
                if(!running||running->IsHardcoreActive())throw std::runtime_error("金手指不可用");
                running->OpenCheats(path);
                return true;
            }catch(const std::exception& error){notify(error.what());return false;}
        };
        actions.editCheat=[](int index,const std::string& name,const std::string& codes){
            try{
                if(!running||running->IsHardcoreActive())return false;
                const auto title=TicoUtils::Trim(name);
                if(title.empty()||title.find_first_of("\r\n")!=std::string::npos)
                    throw std::runtime_error("请输入金手指名称");
                std::string separated=codes;
                std::replace(separated.begin(),separated.end(),'+','\n');
                std::istringstream stream(separated);
                std::vector<std::string> lines;
                for(std::string line;std::getline(stream,line);){
                    line=TicoUtils::Trim(line);
                    if(line.find_first_not_of("0123456789ABCDEFabcdef:- \t")!=std::string::npos)
                        throw std::runtime_error("金手指内容应为十六进制代码");
                    if(!line.empty())lines.push_back(std::move(line));
                }
                if(lines.empty())throw std::runtime_error("请输入金手指内容");
                auto entries=running->GetCheats();
                if(index<0){index=static_cast<int>(entries.size());entries.emplace_back();}
                auto& entry=entries.at(index);
                entry.name=title;entry.codes=std::move(lines);
                running->SaveCheats(std::move(entries));
                return true;
            }catch(const std::exception& error){notify(error.what());return false;}
        };
        actions.pinCheat=[](int index){
            try{
                if(!running||running->IsHardcoreActive())return false;
                auto entries=running->GetCheats();
                auto& entry=entries.at(index);
                entry.pinned=!entry.pinned;
                running->SaveCheats(std::move(entries));
                return true;
            }catch(const std::exception& error){notify(error.what());return false;}
        };
        actions.deleteCheat=[](int index){
            try{
                if(!running||running->IsHardcoreActive())return false;
                auto entries=running->GetCheats();
                if(index<0||index>=static_cast<int>(entries.size()))return false;
                entries.erase(entries.begin()+index);
                running->SaveCheats(std::move(entries));
                return true;
            }catch(const std::exception& error){notify(error.what());return false;}
        };
        actions.readMemory=[]{
            std::vector<menus::MemoryRegion> regions;
            if(running)for(auto& [base,bytes]:running->SearchableMemory())
                regions.push_back({base,std::move(bytes)});
            return regions;
        };
        actions.shaderPresets = presets;
        actions.shaderPresetPath = woplay_host::configuredShaderPath;
        actions.setShaderPreset = [](const std::string& path) { if (shader.select) shader.select(path); };
        actions.shaderParameters = [] {
            std::vector<menus::ShaderParameter> parameters;
            if (shader.parameters)
                for (const auto& entry : shader.parameters())
                    parameters.push_back({entry.id, entry.label, entry.value, entry.minimum, entry.maximum, entry.step});
            return parameters;
        };
        actions.setShaderParameter = [](const std::string& name, float value) {
            if (shader.set_parameter) shader.set_parameter(name, value);
        };
        actions.resetShaderParameters = [] { if (shader.reset_parameters) shader.reset_parameters(); };
        menu = std::make_unique<menus::CoreMenu>(textures, fonts, module, std::move(actions));
        if (!woplay_host::launchingGame()) menu->showSettings();
        return true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "WoPlay menu: %s\n", error.what());
        return false;
    }
}

void visible(bool show) { if (menu) menu->setVisible(show); }
void offerResume() { if (menu) menu->offerResume(); }
void shaders(ShaderCallbacks callbacks) { shader = std::move(callbacks); }
void draw(TicoCore* core, ImVec4 frame) {
    if (!menu) return;
    running = core;
    if (effects) effects->beginFrame();
    ui::ScopedUiStyle config(style);
    ui::ScopedEffects material(effects.get());
    ui::ScopedFocusGradient focus(textures.textureRef(focusGradient));
    menu->draw({woplay_host::title(),{textures.textureRef(gameIcon)},{textures.textureRef(playerAvatar)},
                woplay_host::sessionSeconds(),{{frame.x,frame.y},{frame.x+frame.z,frame.y+frame.w}}});
}

std::optional<ImVec4> previewFrame(ImVec2 size) {
    if (menu) {
        if (const auto frame = menu->previewFrame())
            return ImVec4{frame->min.x*size.x,frame->min.y*size.y,frame->size().x*size.x,frame->size().y*size.y};
    }
    return {};
}

bool gamePointer(ImVec2 position, bool down) {
    return menu && menu->gamePointer(position,down);
}

void prepareEffects(VkCommandBuffer_T* cmd, ImDrawData& frame) {
    if (effects) effects->prepare(cmd,frame);
}

void drawOverlay(ImDrawList& draw, ImVec2 bounds) {
    const auto path = woplay_host::overlayPath();
    if (path != overlayFile) {
        textures.destroyTexture(overlayTexture);
        overlayFile = path;
        overlayTexture = picture(path);
        if (!path.empty() && !overlayTexture.value) notify(translate("无法读取遮罩图片"));
    }
    if (overlayTexture.value)
        draw.AddImage(textures.textureRef(overlayTexture),{0,0},bounds);
}

std::optional<woplay_host::StateRequest> takeStateAction() { return std::exchange(pendingState,{}); }

void hud(float fps, int width, int height, bool fast) {
    constexpr std::array<const char*,4> positions{"top_left","top_right","bottom_left","bottom_right"};
    const auto counter = woplay_host::option("fps_counter_position", "hidden");
    const auto size = woplay_host::option("rendered_ir_position", "hidden");
    const auto canvas = ui::Canvas::current();
    const float scale = canvas.scale;
    ui::ScopedUiStyle config(style);
    auto& draw = *ImGui::GetBackgroundDrawList();
    for (std::size_t index = 0; index < positions.size(); ++index) {
        std::string label;
        if (fast && index == 1) label = ">>";
        if (counter == positions[index]) label += (label.empty() ? "" : "   ") +
            std::to_string(static_cast<int>(fps + .5f)) + " FPS";
        if (size == positions[index] && width > 0 && height > 0) label +=
            (label.empty() ? "" : "   ") + std::to_string(width) + " × " + std::to_string(height);
        if (label.empty()) continue;
        const auto measured = fonts.text->CalcTextSizeA(ui::fontSize(16*scale), 10000, 0, label.c_str());
        const ImVec2 box{measured.x + 28*scale, measured.y + 12*scale};
        const ImVec2 pos{canvas.viewportOrigin.x + (index % 2 ? canvas.viewportSize.x - box.x - 16*scale : 16*scale),
                         canvas.viewportOrigin.y + (index >= 2 ? canvas.viewportSize.y - box.y - 16*scale : 16*scale)};
        ui::Surface background;
        background.themeColor = true;
        background.kind = ui::SurfaceKind::Hud;
        ui::surface(draw, {pos,{pos.x+box.x,pos.y+box.y}}, box.y*.5f, background, scale);
        const auto color = ui::appearance(ui::SurfaceKind::Hud).flatColor;
        const auto ink = color.x*.2126f+color.y*.7152f+color.z*.0722f > .45f ?
            IM_COL32(38,57,72,255) : ui::white;
        ui::text(draw, fonts.text, 16*scale, {pos.x+14*scale,pos.y+6*scale}, ink, label.c_str());
    }
}
void input(SDL_GameController* pad) {
    auto& io = ImGui::GetIO();
    if (pad) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    const auto key = [&](ImGuiKey code, SDL_GameControllerButton button, bool alternate = false) {
        io.AddKeyEvent(code, alternate || (pad && SDL_GameControllerGetButton(pad, button)));
    };
    if (menu) {
        const auto axis = [pad](SDL_GameControllerAxis axis) {
            return pad ? SDL_GameControllerGetAxis(pad,axis)/32768.F : 0.F;
        };
        menu->setDisplayPanelStick({axis(SDL_CONTROLLER_AXIS_RIGHTX),axis(SDL_CONTROLLER_AXIS_RIGHTY)});
    }
    const auto x = pad ? SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) : 0;
    const auto y = pad ? SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) : 0;
    const bool capture=menu&&menu->bindingListening();
    key(ImGuiKey_GamepadDpadUp, SDL_CONTROLLER_BUTTON_DPAD_UP, !capture&&y < -16000);
    key(ImGuiKey_GamepadDpadDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN, !capture&&y > 16000);
    key(ImGuiKey_GamepadDpadLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT, !capture&&x < -16000);
    key(ImGuiKey_GamepadDpadRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, !capture&&x > 16000);
    const bool nintendo=
#ifdef __SWITCH__
        true;
#else
        capture;
#endif
    key(ImGuiKey_GamepadFaceDown,nintendo?SDL_CONTROLLER_BUTTON_B:SDL_CONTROLLER_BUTTON_A);
    key(ImGuiKey_GamepadFaceRight,nintendo?SDL_CONTROLLER_BUTTON_A:SDL_CONTROLLER_BUTTON_B);
    key(ImGuiKey_GamepadFaceUp,SDL_CONTROLLER_BUTTON_Y);
    key(ImGuiKey_GamepadFaceLeft,SDL_CONTROLLER_BUTTON_X);
    key(ImGuiKey_GamepadL1, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    key(ImGuiKey_GamepadR1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    key(ImGuiKey_GamepadL3,SDL_CONTROLLER_BUTTON_LEFTSTICK);
    key(ImGuiKey_GamepadR3,SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    key(ImGuiKey_GamepadStart,SDL_CONTROLLER_BUTTON_START);
    key(ImGuiKey_GamepadBack,SDL_CONTROLLER_BUTTON_BACK);
    const auto axis=[&](SDL_GameControllerAxis line){return pad?SDL_GameControllerGetAxis(pad,line):0;};
    io.AddKeyEvent(ImGuiKey_GamepadL2,axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT)>16000);
    io.AddKeyEvent(ImGuiKey_GamepadR2,axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>16000);
    io.AddKeyEvent(ImGuiKey_GamepadLStickUp,capture&&y<-16000);
    io.AddKeyEvent(ImGuiKey_GamepadLStickDown,capture&&y>16000);
    io.AddKeyEvent(ImGuiKey_GamepadLStickLeft,capture&&x<-16000);
    io.AddKeyEvent(ImGuiKey_GamepadLStickRight,capture&&x>16000);
    io.AddKeyEvent(ImGuiKey_GamepadRStickUp,capture&&axis(SDL_CONTROLLER_AXIS_RIGHTY)<-16000);
    io.AddKeyEvent(ImGuiKey_GamepadRStickDown,capture&&axis(SDL_CONTROLLER_AXIS_RIGHTY)>16000);
    io.AddKeyEvent(ImGuiKey_GamepadRStickLeft,capture&&axis(SDL_CONTROLLER_AXIS_RIGHTX)<-16000);
    io.AddKeyEvent(ImGuiKey_GamepadRStickRight,capture&&axis(SDL_CONTROLLER_AXIS_RIGHTX)>16000);
}
bool bindingActive() { return menu&&menu->bindingActive(); }
void touch(const SwitchFrontend::OverlayUI::TouchInput& input) {
    if (gamePointer({input.x,input.y},input.down)) return;
    auto& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    if (input.down) io.AddMousePosEvent(input.x, input.y);
    io.AddMouseButtonEvent(0, input.down);
}
void notify(const std::string& text) { if (menu) menu->notify(text); }
Action takeAction() { return std::exchange(pending, Action::None); }
void shutdown() {
    menu.reset();
    effects.reset();
    textures.shutdown();
    shader = {};
    running = nullptr;
}
}
