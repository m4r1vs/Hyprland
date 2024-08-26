#include "Popup.hpp"
#include "../config/ConfigValue.hpp"
#include "../Compositor.hpp"
#include "../protocols/LayerShell.hpp"
#include "../protocols/XDGShell.hpp"
#include "../protocols/core/Compositor.hpp"
#include <ranges>

CPopup::CPopup(PHLWINDOW pOwner) : m_pWindowOwner(pOwner) {
    initAllSignals();
}

CPopup::CPopup(PHLLS pOwner) : m_pLayerOwner(pOwner) {
    initAllSignals();
}

CPopup::CPopup(SP<CXDGPopupResource> popup, CPopup* pOwner) : m_pParent(pOwner), m_pResource(popup) {
    m_pWLSurface = CWLSurface::create();
    m_pWLSurface->assign(popup->surface->surface.lock(), this);

    m_pLayerOwner  = pOwner->m_pLayerOwner;
    m_pWindowOwner = pOwner->m_pWindowOwner;

    m_vLastSize = popup->surface->current.geometry.size();
    reposition();

    initAllSignals();
}

CPopup::~CPopup() {
    if (m_pWLSurface)
        m_pWLSurface->unassign();
}

void CPopup::initAllSignals() {

    if (!m_pResource) {
        if (!m_pWindowOwner.expired())
            listeners.newPopup = m_pWindowOwner->m_pXDGSurface->events.newPopup.registerListener([this](std::any d) { this->onNewPopup(std::any_cast<SP<CXDGPopupResource>>(d)); });
        else if (!m_pLayerOwner.expired())
            listeners.newPopup = m_pLayerOwner->layerSurface->events.newPopup.registerListener([this](std::any d) { this->onNewPopup(std::any_cast<SP<CXDGPopupResource>>(d)); });
        else
            ASSERT(false);

        return;
    }

    listeners.reposition = m_pResource->events.reposition.registerListener([this](std::any d) { this->onReposition(); });
    listeners.map        = m_pResource->surface->events.map.registerListener([this](std::any d) { this->onMap(); });
    listeners.unmap      = m_pResource->surface->events.unmap.registerListener([this](std::any d) { this->onUnmap(); });
    listeners.dismissed  = m_pResource->events.dismissed.registerListener([this](std::any d) { this->onUnmap(); });
    listeners.destroy    = m_pResource->surface->events.destroy.registerListener([this](std::any d) { this->onDestroy(); });
    listeners.commit     = m_pResource->surface->events.commit.registerListener([this](std::any d) { this->onCommit(); });
    listeners.newPopup   = m_pResource->surface->events.newPopup.registerListener([this](std::any d) { this->onNewPopup(std::any_cast<SP<CXDGPopupResource>>(d)); });
}

void CPopup::onNewPopup(SP<CXDGPopupResource> popup) {
    const auto POPUP = m_vChildren.emplace_back(makeShared<CPopup>(popup, this)).get();
    Debug::log(LOG, "New popup at {:x}", (uintptr_t)POPUP);
}

void CPopup::onDestroy() {
    m_bInert = true;

    if (!m_pParent)
        return; // head node

    std::erase_if(m_pParent->m_vChildren, [this](const auto& other) { return other.get() == this; });
}

void CPopup::onMap() {
    if (m_bMapped)
        return;

    m_bMapped   = true;
    m_vLastSize = m_pResource->surface->surface->current.size;

    const auto COORDS   = coordsGlobal();
    const auto PMONITOR = g_pCompositor->getMonitorFromVector(COORDS);

    CBox       box = m_pWLSurface->resource()->extends();
    box.translate(COORDS).expand(4);
    g_pHyprRenderer->damageBox(&box);

    m_vLastPos = coordsRelativeToParent();

    g_pInputManager->simulateMouseMovement();

    m_pSubsurfaceHead = std::make_unique<CSubsurface>(this);

    //unconstrain();
    sendScale();
    m_pResource->surface->surface->enter(PMONITOR->self.lock());

    if (!m_pLayerOwner.expired() && m_pLayerOwner->layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
        g_pHyprOpenGL->markBlurDirtyForMonitor(g_pCompositor->getMonitorFromID(m_pLayerOwner->layer));
}

void CPopup::onUnmap() {
    if (!m_bMapped)
        return;

    if (!m_pResource || !m_pResource->surface) {
        Debug::log(ERR, "CPopup: orphaned (no surface/resource) and unmaps??");
        onDestroy();
        return;
    }

    m_vLastSize = m_pResource->surface->surface->current.size;

    const auto COORDS = coordsGlobal();

    CBox       box = m_pWLSurface->resource()->extends();
    box.translate(COORDS).expand(4);
    g_pHyprRenderer->damageBox(&box);

    m_pSubsurfaceHead.reset();

    g_pInputManager->simulateMouseMovement();

    if (!m_pLayerOwner.expired() && m_pLayerOwner->layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
        g_pHyprOpenGL->markBlurDirtyForMonitor(g_pCompositor->getMonitorFromID(m_pLayerOwner->layer));

    // damage all children
    breadthfirst(
        [](CPopup* p, void* data) {
            if (!p->m_pResource)
                return;

            auto box = CBox{p->coordsGlobal(), p->size()};
            g_pHyprRenderer->damageBox(&box);
        },
        nullptr);
}

void CPopup::onCommit(bool ignoreSiblings) {
    if (!m_pResource || !m_pResource->surface) {
        Debug::log(ERR, "CPopup: orphaned (no surface/resource) and commits??");
        onDestroy();
        return;
    }

    if (m_pResource->surface->initialCommit) {
        m_pResource->surface->scheduleConfigure();
        return;
    }

    if (!m_pWindowOwner.expired() && (!m_pWindowOwner->m_bIsMapped || !m_pWindowOwner->m_pWorkspace->m_bVisible)) {
        m_vLastSize = m_pResource->surface->surface->current.size;

        static auto PLOGDAMAGE = CConfigValue<Hyprlang::INT>("debug:log_damage");
        if (*PLOGDAMAGE)
            Debug::log(LOG, "Refusing to commit damage from a subsurface of {} because it's invisible.", m_pWindowOwner.lock());
        return;
    }

    if (!m_pResource->surface->mapped)
        return;

    const auto COORDS      = coordsGlobal();
    const auto COORDSLOCAL = coordsRelativeToParent();

    if (m_vLastSize != m_pResource->surface->surface->current.size || m_bRequestedReposition || m_vLastPos != COORDSLOCAL) {
        CBox box = {localToGlobal(m_vLastPos), m_vLastSize};
        g_pHyprRenderer->damageBox(&box);
        m_vLastSize = m_pResource->surface->surface->current.size;
        box         = {COORDS, m_vLastSize};
        g_pHyprRenderer->damageBox(&box);

        m_vLastPos = COORDSLOCAL;
    }

    if (!ignoreSiblings && m_pSubsurfaceHead)
        m_pSubsurfaceHead->recheckDamageForSubsurfaces();

    g_pHyprRenderer->damageSurface(m_pWLSurface->resource(), COORDS.x, COORDS.y);

    m_bRequestedReposition = false;

    if (!m_pLayerOwner.expired() && m_pLayerOwner->layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
        g_pHyprOpenGL->markBlurDirtyForMonitor(g_pCompositor->getMonitorFromID(m_pLayerOwner->layer));
}

void CPopup::onReposition() {
    Debug::log(LOG, "Popup {:x} requests reposition", (uintptr_t)this);

    m_bRequestedReposition = true;

    m_vLastPos = coordsRelativeToParent();

    reposition();
}

void CPopup::reposition() {
    const auto COORDS   = t1ParentCoords();
    const auto PMONITOR = g_pCompositor->getMonitorFromVector(COORDS);

    if (!PMONITOR)
        return;

    CBox box = {PMONITOR->vecPosition.x, PMONITOR->vecPosition.y, PMONITOR->vecSize.x, PMONITOR->vecSize.y};
    m_pResource->applyPositioning(box, COORDS);
}

Vector2D CPopup::coordsRelativeToParent() {
    Vector2D offset;

    if (!m_pResource)
        return {};

    CPopup* current = this;
    offset -= current->m_pResource->surface->current.geometry.pos();

    while (current->m_pParent && current->m_pResource) {

        offset += current->m_pWLSurface->resource()->current.offset;
        offset += current->m_pResource->geometry.pos();

        current = current->m_pParent;
    }

    return offset;
}

Vector2D CPopup::coordsGlobal() {
    return localToGlobal(coordsRelativeToParent());
}

Vector2D CPopup::localToGlobal(const Vector2D& rel) {
    return t1ParentCoords() + rel;
}

Vector2D CPopup::t1ParentCoords() {
    if (!m_pWindowOwner.expired())
        return m_pWindowOwner->m_vRealPosition.value();
    if (!m_pLayerOwner.expired())
        return m_pLayerOwner->realPosition.value();

    ASSERT(false);
    return {};
}

void CPopup::recheckTree() {
    CPopup* curr = this;
    while (curr->m_pParent) {
        curr = curr->m_pParent;
    }

    curr->recheckChildrenRecursive();
}

void CPopup::recheckChildrenRecursive() {
    auto cpy = m_vChildren;
    for (auto const& c : cpy) {
        c->onCommit(true);
        c->recheckChildrenRecursive();
    }
}

Vector2D CPopup::size() {
    return m_vLastSize;
}

void CPopup::sendScale() {
    if (!m_pWindowOwner.expired())
        g_pCompositor->setPreferredScaleForSurface(m_pWLSurface->resource(), m_pWindowOwner->m_pWLSurface->m_fLastScale);
    else if (!m_pLayerOwner.expired())
        g_pCompositor->setPreferredScaleForSurface(m_pWLSurface->resource(), m_pLayerOwner->surface->m_fLastScale);
    else
        UNREACHABLE();
}

bool CPopup::visible() {
    if (!m_pWindowOwner.expired())
        return g_pHyprRenderer->shouldRenderWindow(m_pWindowOwner.lock());
    if (!m_pLayerOwner.expired())
        return true;
    if (m_pParent)
        return m_pParent->visible();

    return false;
}

void CPopup::bfHelper(std::vector<CPopup*> nodes, std::function<void(CPopup*, void*)> fn, void* data) {
    for (auto const& n : nodes) {
        fn(n, data);
    }

    std::vector<CPopup*> nodes2;

    for (auto const& n : nodes) {
        for (auto const& c : n->m_vChildren) {
            nodes2.push_back(c.get());
        }
    }

    if (!nodes2.empty())
        bfHelper(nodes2, fn, data);
}

void CPopup::breadthfirst(std::function<void(CPopup*, void*)> fn, void* data) {
    std::vector<CPopup*> popups;
    popups.push_back(this);
    bfHelper(popups, fn, data);
}

CPopup* CPopup::at(const Vector2D& globalCoords, bool allowsInput) {
    std::vector<CPopup*> popups;
    breadthfirst([](CPopup* popup, void* data) { ((std::vector<CPopup*>*)data)->push_back(popup); }, &popups);

    for (auto const& p : popups | std::views::reverse) {
        if (!p->m_pResource)
            continue;

        if (!allowsInput) {
            const Vector2D offset = p->m_pResource ? (p->size() - p->m_pResource->geometry.size()) / 2.F : Vector2D{};
            const Vector2D size   = p->m_pResource ? p->m_pResource->geometry.size() : p->size();

            const auto     BOX = CBox{p->coordsGlobal() + offset, size};
            if (BOX.containsPoint(globalCoords))
                return p;
        } else {
            const Vector2D offset = p->m_pResource ? (p->size() - p->m_pResource->geometry.size()) / 2.F : Vector2D{};
            const auto     REGION =
                CRegion{p->m_pWLSurface->resource()->current.input}.intersect(CBox{{}, p->m_pWLSurface->resource()->current.size}).translate(p->coordsGlobal() + offset);
            if (REGION.containsPoint(globalCoords))
                return p;
        }
    }

    return nullptr;
}
