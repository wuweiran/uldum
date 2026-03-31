#pragma once

namespace uldum::script { class ScriptEngine; }

namespace uldum::simulation {

class Simulation {
public:
    bool init(script::ScriptEngine& script);
    void shutdown();
    void tick(float dt);

    // Future API:
    // Entity create_entity();
    // void destroy_entity(Entity e);
    // template<typename T> T& add_component(Entity e);
    // template<typename T> T& get_component(Entity e);
    //
    // Unit-centric facade (exposed to Lua):
    // UnitHandle create_unit(UnitTypeId type, PlayerId owner, float x, float y);
    // void unit_add_ability(UnitHandle unit, AbilityTypeId ability);
    // void issue_order(UnitHandle unit, Order order);
};

} // namespace uldum::simulation
