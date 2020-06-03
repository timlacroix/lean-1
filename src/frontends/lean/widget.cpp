/*
Copyright (c) E.W.Ayers. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: E.W.Ayers
*/
#include <map>
#include <vector>
#include <string>
#include <atomic>
#include "library/vm/vm.h"
#include "library/vm/vm_option.h"
#include "library/vm/vm_string.h"
#include "library/vm/vm_list.h"
#include "library/vm/vm_task.h"
#include "util/list.h"
#include "frontends/lean/widget.h"
#include "frontends/lean/json.h"
#include "util/optional.h"
#include "util/pair.h"

namespace lean {

std::atomic_uint g_fresh_handler_id;
std::atomic_uint g_fresh_component_instance_id;

optional<std::string> vdom_element::key() {
    if (m_attrs.find("key") != m_attrs.end()) {
        // there is an entry with key "key"
        std::string k = m_attrs["key"];
        return optional<std::string>(k);
    }
    return optional<std::string>();
}
void vdom_element::reconcile(vdom const & old_vdom) {
    vdom_element * o = dynamic_cast<vdom_element*>(old_vdom.raw());
    if (o && o->m_tag == m_tag) {
        std::vector<vdom> ocs = o->m_children;
        reconcile_children(m_children, ocs);
        if (m_tooltip && o->m_tooltip) {
            (*m_tooltip).reconcile(*(o->m_tooltip));
        }
    }
}
json vdom_element::to_json(list<unsigned> const & route) {
    json entry;
    entry["t"] = m_tag;
    entry["a"] = m_attrs;
    json jr = json::array();
    for (auto const & i : route) {
        jr.push_back(i);
    }
    for (auto const & x : m_events) {
        entry["e"][x.first]["r"] = jr;
        entry["e"][x.first]["h"] = json(x.second);
    }
    entry["c"] = json::array();
    for (vdom & v : m_children) {
        entry["c"].push_back(v.to_json(route));
    }
    if (m_tooltip) {
        entry["tt"] = (*m_tooltip).to_json(route);
    }
    return entry;
}

component_instance::component_instance(vm_obj const & c, vm_obj const & props, list<unsigned> const & route):
  m_component(c), m_props(props), m_route(route) {
      m_id = g_fresh_component_instance_id.fetch_add(1);
      m_reconcile_count = 0;
      m_component_hash = hash(c);
}

vm_obj stateful::init(vm_obj const & p, optional<vm_obj> const & s) {
    vm_obj os = s ? mk_vm_some(*s) : mk_vm_none();
    return invoke(cfield(m_component.to_vm_obj(), 0), p, os);
}

pair<vm_obj, optional<vm_obj>> stateful::update(vm_obj const & p, vm_obj const & s, vm_obj const & a) {
    vm_obj sa = invoke(cfield(m_component.to_vm_obj(), 1), p, s, a);
    vm_obj oa = cfield(sa, 1);
    optional<vm_obj> o = is_none(oa) ? optional<vm_obj>() : optional<vm_obj>(get_some_value(oa));
    return mk_pair(cfield(sa, 0), o);
}

vm_obj stateful::view(vm_obj const & p, vm_obj const & s) {
    return invoke(cfield(m_component.to_vm_obj(), 2), p, s);
}

bool stateful::props_are_equal(vm_obj const & p_old, vm_obj const & p_new) {
    return to_bool(invoke(cfield(m_component.to_vm_obj(), 3), p_old, p_new));
}

void stateful::render() {
    std::vector<component_instance *> children;
    std::map<unsigned, ts_vm_obj> handlers;
    std::vector<vdom> elements = render_html_list(view(m_props.to_vm_obj(), (*m_state).to_vm_obj()), children, handlers, cons(m_id, m_route));
    std::vector<vdom> old_elements = m_render;
    reconcile_children(elements, old_elements);
    m_handlers = handlers;
    m_children = children;
    m_render = elements;
    m_has_rendered = true;
}

void delayed::render() {
    vm_obj child_comp = cfield(m_component.to_vm_obj(),1);
    lean_assert(m_task);
    optional<ts_vm_obj> result = peek(*m_task);
    vm_obj props = mk_vm_pair(m_props.to_vm_obj(), result ? mk_vm_some(result->to_vm_obj()) : mk_vm_none());
    component_instance * new_child = new_component_instance(child_comp, props, child_route());
    std::vector<vdom> elements = {vdom(new_child)};
    std::vector<vdom> old_elements = m_render;
    reconcile_children(elements, old_elements);
    m_children = {new_child};
    m_render = elements;
    m_has_rendered = true;
}

void component_instance::reconcile(vdom const & old) {
    lean_assert(!m_has_rendered);
    component_instance * ci_old = dynamic_cast<component_instance *>(old.raw());
    // If they contain vm_externals which are not hashable then we assume they are the same component.
    // This is acceptable, but confusing, behaviour for now. It just means that the component won't always
    // update correctly if a non-prop dependency of a component changes.
    // But users of components should be using Props anyway so there is a workaround.
    if (ci_old && ci_old->m_component_hash == m_component_hash) {
        // if the components are the same:
        // note that this doesn't occur if they do the same thing but were made with different calls to component.mk.
        vm_obj p_new = m_props.to_vm_obj();
        vm_obj p_old = ci_old->m_props.to_vm_obj();
        if (p_new == p_old || props_are_equal(p_old, p_new)) {
            // the props are equal and the state didn't change, so we can just keep the old rendering.
            m_children = ci_old->m_children;
            m_render   = ci_old->m_render;
            m_id       = ci_old->m_id;
            m_has_rendered = true;
            m_reconcile_count = ci_old->m_reconcile_count + 1;
            lean_assert(m_route == ci_old->m_route);
            carry(ci_old);
        } else {
            // the props have changed, so we need to rerender this component.
            // we use `init` to recompute the state.
            props_changed(ci_old);
            render();
        }
    } else {
        // The old component is completely different, so render as a fresh component.
        initialize();
        render();
    }
}

void stateful::initialize() {
    if (m_state) {return; }
    m_state = some<ts_vm_obj>(init(m_props.to_vm_obj(), optional<vm_obj>()))
}

void stateful::props_changed(component_instance * old) {
    stateful * ci_old = dynamic_cast<stateful *>(old);
    lean_assert(ci_old);
    optional<vm_obj> old_state = some((*(ci_old->m_state)).to_vm_obj());
    ts_vm_obj new_state = init(m_props.to_vm_obj(), old_state);
    m_state = optional<ts_vm_obj>(new_state);
}

void stateful::carry(component_instance * old) {
    stateful * ci_old = dynamic_cast<stateful *>(old);
    lean_assert(ci_old);
    m_state    = ci_old->m_state;
    m_handlers = ci_old->m_handlers;
}

void delayed::initialize() {
    if (m_task) {return; }
    vm_obj vt = invoke(cfield(m_component.to_vm_obj(),0), m_props.to_vm_obj());
    task<ts_vm_obj> t = to_task(vt);
    m_task = optional<task<ts_vm_obj>>(t);
    taskq().submit(t);
    unsigned handler_id = g_fresh_handler_id.fetch_add(1);
    m_handler = handler_id;
    auto route = cons(m_id, m_route);
    pending_tasks().push_back(task_builder<list<unsigned>>([route] {
        return route;
    }).depends_on(t).build());
}

void delayed::carry(component_instance * old) {
    delayed * ci_old = dynamic_cast<delayed *>(old);
    lean_assert(ci_old);
    m_task = ci_old->m_task;
}

void delayed::props_changed(component_instance * old) {
    delayed * ci_old = dynamic_cast<delayed *>(old);
    // cancel the old task, hopefully any task queue stuff will get removed eventually.
    if (ci_old->m_task) {
        task<ts_vm_obj> t = *(ci_old->m_task);
        taskq().fail_and_dispose(t);
    }
    initialize();
}

delayed::~delayed() {
    if (m_task) {
        taskq().fail_and_dispose(*m_task); //hopefully this doesn't error if it's already disposed.
    }
}

void delayed::reconcile(vdom const & old) {
    lean_assert(!m_has_rendered);
    delayed * ci_old = dynamic_cast<delayed *>(old.raw());
    if (ci_old && ci_old->m_component_hash == m_component_hash) {
        vm_obj p_new = m_props.to_vm_obj();
        vm_obj p_old = ci_old->m_props.to_vm_obj()
    } else {
        // old component is different.
        render();
    }
}

json component_instance::to_json(list<unsigned> const & route) {
    if (!m_has_rendered) {
        initialize();
        render();
    }
    json children = json::array();
    for (vdom const & x : m_render) {
        children.push_back(x.to_json(cons(m_id, route)));
    }
    json result;
    result["c"] = children;
    return result;
}

optional<vm_obj> stateful::handle_action(vm_obj const & a) {
    auto p = update(m_props.to_vm_obj(), (*m_state).to_vm_obj(), a);
    m_state = p.first;
    render();
    return p.second;
}

optional<vm_obj> delayed::handle_action(vm_obj const & a) {
    // note no need to rerender because nothing changed.
    return some<vm_obj>(a);
}

optional<vm_obj> stateful::handle_event_core(unsigned handler_id, vm_obj const & event_args) {
        if (m_handlers.find(handler_id) == m_handlers.end()) {
            // component may have rerendered, but handler_id refers to event handler on older component.
            throw invalid_handler();
        }
        ts_vm_obj handler = m_handlers[handler_id];
        // [todo] to prevent a VM error in the case of bad client code, we should check that the 'type' of the event_args here is the same as what the handler expects.
        // the event record from the client should have a `type` member which can be checked. So each of `m_handlers` should also include a string 'type' to check against.
        auto action = invoke(handler.to_vm_obj(), event_args);
        return handle_action(action);
}

optional<vm_obj> delayed::handle_event_core(unsigned handler_id, vm_obj const &) {
    // this should be triggered when the task completes.
    // perform a valid handler sanity check on handler_id.
    render();
    return optional<vm_obj>();
}

optional<vm_obj> component_instance::handle_event(list<unsigned> const & route, unsigned handler_id, vm_obj const & event_args) {
    if (empty(route)) {
        return handle_event_core(handler_id, event_args);
    }
    for (auto const & c : m_children) {
        if (c->m_id == head(route)) {
            optional<vm_obj> a = c->handle_event(tail(route), handler_id, event_args);
            if (a) {
                return handle_action(*a);
            } else {
                return optional<vm_obj>();
            }
        }
    }
    // given component no longer exists. This happens if the ui is updated but there are events from an old vdom
    throw invalid_handler();
}

void reconcile_children(std::vector<vdom> & new_elements, std::vector<vdom> const & olds) {
    std::vector<vdom> old_elements = olds;
    for (unsigned i = 0; i < new_elements.size(); i++) {
        // [note] you get weird behaviour if multiple things have the same key or if only some elements have keys
        // but this is also true in React so I am not too worried about it as long as it doesn't crash.
        // [todo] add a warning if keys are duplicated or only present on some objects.
        auto k = new_elements[i].key();
        if (k) {
            for (unsigned j = 0; j < old_elements.size(); j++) {
                if (old_elements[j].key() == k) {
                    vdom o = old_elements[j];
                    new_elements[i].reconcile(o);
                    old_elements.erase(old_elements.begin() + j);
                    break;
                }
            }
        } else if (old_elements.size() > 0) {
            new_elements[i].reconcile(old_elements[0]);
            old_elements.erase(old_elements.begin());
        } else {
            // continue
        }
    }
}


void render_event(std::string const & name, vm_obj const & handler, std::map<std::string, unsigned> & events, std::map<unsigned, ts_vm_obj> & handlers) {
    unsigned handler_id = g_fresh_handler_id.fetch_add(1);
    events[name] = handler_id;
    handlers[handler_id] = handler;
}

vdom render_element(vm_obj const & elt, std::vector<component_instance*> & components, std::map<unsigned, ts_vm_obj> & handlers, list<unsigned> const & route) {
    // | element      {α : Type} (tag : string) (attrs : list (attr α)) (children : list (html α)) : html α
    std::string tag = to_string(cfield(elt, 0));
    vm_obj v_attrs = cfield(elt, 1);
    vm_obj v_children = cfield(elt, 2);
    json attributes;
    std::map<std::string, unsigned> events;
    optional<vdom> tooltip;
    while (!is_simple(v_attrs)) {
        vm_obj attr = head(v_attrs);
        v_attrs = tail(v_attrs);
        switch (cidx(attr)) {
            case 5: { // val {\a} : string -> string -> attr
                std::string key = to_string(cfield(attr, 0));
                std::string value = to_string(cfield(attr, 1));
                // [note] className fields should be merged.
                if (key == "className" && attributes.find(key) != attributes.end()) {
                    std::string cn = attributes[key];
                    cn += " ";
                    cn += value;
                    attributes[key] = cn;
                } else {
                    attributes[key] = value;
                }
                break;
            } case 6: {// on_mouse_event {\a} : mouse_event_kind -> (unit -> Action) -> html.attr
                int mouse_event_kind = cidx(cfield(attr, 0));
                vm_obj handler = cfield(attr, 1);
                switch (mouse_event_kind) {
                    case 0: render_event("onClick",      handler, events, handlers); break;
                    case 1: render_event("onMouseEnter", handler, events, handlers); break;
                    case 2: render_event("onMouseLeave", handler, events, handlers); break;
                    default: lean_unreachable();
                }
                break;
            } case 7: { // style {a} : list (string × string) → html.attr
                auto l = cfield(attr, 0);
                while (!is_simple(l)) {
                    auto h = head(l);
                    auto k = to_string(cfield(h, 0));
                    auto v = to_string(cfield(h, 1));
                    attributes["style"][k] = v;
                    l = tail(l);
                }
                break;
            } case 8: { // tooltip {a} :  html Action → html.attr
                auto content = cfield(attr, 0);
                vdom tooltip_child = render_html(content, components, handlers, route);
                tooltip = optional<vdom>(tooltip_child);
                break;
            } case 9: { // text_change_event {a} : (string -> Action) -> html.attr
                auto handler = cfield(attr, 0);
                render_event("onChange", handler, events, handlers);
                break;
            } default : {
                lean_unreachable();
                break;
            }
        }
    }
    std::vector<vdom> children = render_html_list(v_children, components, handlers, route);
    return vdom(new vdom_element(tag, attributes, events, children, tooltip));
}

component_instance * new_component_instance(vm_obj const & comp, vm_obj const & props, list<unsigned> const & route) {
    switch(cidx(comp)) {
        case 0: // stateful
            return new stateful(comp, props, route);
        case 1: // delayed
            return new delayed(comp, props, route);
        default:
            lean_unreachable();
    }
}

vdom render_html(vm_obj const & html, std::vector<component_instance*> & components, std::map<unsigned, ts_vm_obj> & handlers, list<unsigned> const & route) {
    switch (cidx(html)) {
        case 2: { // | of_element {α : Type} (tag : string) (attrs : list (attr α)) (children : list (html α)) : html α
            vdom elt = render_element(html, components, handlers, route);
            return elt;
        } case 3: { // | of_string    {α : Type} : string → html α
            return vdom(new vdom_string(to_string(cfield(html, 0))));
        } case 4: { // | of_component {α : Type} {Props : Type} : Props → component Props α → html α
            vm_obj props = cfield(html, 0);
            vm_obj comp  = cfield(html, 1);
            component_instance * c = new_component_instance(comp, props, route);
            components.push_back(c);
            return vdom(c);
        } default: {
            lean_unreachable();
        }
    }
}

std::vector<vdom> render_html_list(vm_obj const & htmls, std::vector<component_instance*> & components, std::map<unsigned, ts_vm_obj> & handlers, list<unsigned> const & route) {
    std::vector<vdom> elements;
    vm_obj l = htmls;
    while (!is_simple(l)) {
        vdom x = render_html(head(l), components, handlers, route);
        elements.push_back(x);
        l = tail(l);
    }
    return elements;
}

void initialize_widget() {}

void finalize_widget() {}


static pending_tasks * g_pending_tasks = nullptr;
void set_pending_tasks(pending_tasks * q) {
    if (g_pending_tasks) throw exception("cannot set task queue twice");
    g_pending_tasks = q;
}
void unset_pending_tasks() {
    g_pending_tasks = nullptr;
}
pending_tasks & get_pending_tasks() {
    return *g_pending_tasks;
}

}