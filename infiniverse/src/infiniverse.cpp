#include "infiniverse.hpp"
#include "lat_long_functions.cpp"

const uint32_t seconds_in_one_year = 60 * 60 * 24 * 365;
const uint32_t max_land_length = 100;
const symbol inf_symbol = symbol("INF", 4);
const name inf_account = "infinicoinio"_n;
const uint32_t inf_per_sqm = 10;

void infiniverse::registerland(name owner, double lat_north_edge,
    double long_east_edge, double lat_south_edge, double long_west_edge)
{
    require_auth(owner);

    eosio_assert(lat_north_edge > lat_south_edge, "North edge must have greater latitude than south edge");
    // Temporary restriction of registering land across the antimeridian to simplify land intersection algorithm
    eosio_assert(long_east_edge > long_west_edge, "East edge must have greater longitude than west edge");
    // Temporary longitude limit to between -85 and 85 degrees to simplify display of lands on a mapping UI
    eosio_assert(lat_north_edge < 85, "Latitude cannot be greater than 85 degrees");
    eosio_assert(lat_south_edge > -85, "Latitude cannot be less than -85 degrees");
    eosio_assert(long_east_edge <= 180 && long_east_edge > -180 && long_west_edge <= 180
        && long_west_edge > -180, "Longitude must be between -180 and 180 degrees");

    std::pair<double, double> land_size = lat_long_to_meters(lat_north_edge, lat_south_edge,
        long_east_edge, long_west_edge);

    eosio_assert(land_size.first <= max_land_length && land_size.second <= max_land_length,
        ("Land cannot exceed a length of " + std::to_string(max_land_length) + " meters on either side").c_str());

    land_table lands(_self, _self.value);

    auto lat_north_index = lands.get_index<"bylatnorth"_n>();
    auto lands_itr = lat_north_index.lower_bound(lat_south_edge);
    // Add max_land_length meters to lat_north_edge to get upper bound
    double upper_bound = lat_north_edge + meters_to_lat_dist(max_land_length);

    while(lands_itr != lat_north_index.end() && lands_itr->lat_north_edge < upper_bound)
    {
        eosio_assert(
            lands_itr->long_east_edge <= long_west_edge ||
            lands_itr->long_west_edge >= long_east_edge ||
            lands_itr->lat_south_edge >= lat_north_edge ||
            // Required because lower bound includes equality case
            lands_itr->lat_north_edge <= lat_south_edge,
            "Intersecting land has already been registered");
        lands_itr++;
    }

    // Calculate registration fee assuming each side is at least 1 meter to avoid abuse
    // Otherwise a malicious user could register a very thin, long and cheap land
    // This land would be useless but would stop anyone else from registering land over it
    double land_area = std::max(land_size.first, 1.0) * std::max(land_size.second, 1.0);
    int64_t reg_fee = static_cast<int64_t>(round(land_area * inf_per_sqm));
    // multiply fee by 10000 to account for four decimal places of INF
    asset inf_amount = asset(reg_fee * 10000, inf_symbol);

    deposit_table deposits(_self, _self.value);
    auto deposits_itr = deposits.find(owner.value);
    eosio_assert(deposits_itr != deposits.end(), "User does not have a deposit opened");
    eosio_assert(deposits_itr->balance >= inf_amount, "User's INF deposit balance is too low");

    deposits.modify(deposits_itr, same_payer, [&](auto &row){
        row.balance -= inf_amount;
    });

    // The registration fee gets sent back to the token issuing account
    transfer_inf(_self, inf_account, inf_amount, "");

    lands.emplace(owner, [&](auto &row) {
        row.id = lands.available_primary_key();
        row.owner = owner;
        row.lat_north_edge = lat_north_edge;
        row.long_east_edge = long_east_edge;
        row.lat_south_edge = lat_south_edge;
        row.long_west_edge = long_west_edge;
        row.reg_end_date = time_point_sec(now() + seconds_in_one_year);
    });
}

void infiniverse::persistpoly(uint64_t land_id, std::string poly_id,
        vector3 position, vector3 orientation, vector3 scale)
{
    name user = require_land_owner_auth(land_id);
    assert_vectors_within_bounds(position, orientation, scale);

    uint64_t source = static_cast<uint64_t>(PlacementSource::POLY);
    uint64_t asset_id = add_poly(user, poly_id);

    // Pack the source and asset id into one int to store the composite index
    uint128_t source_and_asset_id = (uint128_t) source << 64 | asset_id;

    persistent_table persistents(_self, _self.value);
    persistents.emplace(user, [&](auto &row) {
        row.id = persistents.available_primary_key();
        row.land_id = land_id;
        row.source_and_asset_id = source_and_asset_id;
        row.position = position;
        row.orientation = orientation;
        row.scale = scale;
    });
}

void infiniverse::updatepersis(uint64_t persistent_id, uint64_t land_id,
    vector3 position, vector3 orientation, vector3 scale)
{
    persistent_table persistents(_self, _self.value);
    auto persistents_itr = persistents.find(persistent_id);
    uint64_t old_land_id = get_land_id_from_persistent(persistents, persistent_id);
    require_land_owner_auth(old_land_id);
    if(land_id != old_land_id)
    {
        require_land_owner_auth(land_id);
    }
    assert_vectors_within_bounds(position, orientation, scale);
    persistents.modify(persistents_itr, same_payer, [&](auto &row) {
        row.land_id = land_id;
        row.position = position;
        row.orientation = orientation;
        row.scale = scale;
    });
}

void infiniverse::deletepersis(uint64_t persistent_id)
{
    persistent_table persistents(_self, _self.value);
    auto persistents_itr = persistents.find(persistent_id);
    uint64_t land_id = get_land_id_from_persistent(persistents, persistent_id);
    name user = require_land_owner_auth(land_id);
    uint128_t source_and_asset_id = persistents_itr->source_and_asset_id;
    persistents.erase(persistents_itr);

    // Get the source by unpacking the most significant bits from the composite index
    uint64_t source = (uint64_t)(source_and_asset_id >> 64);
    // If this is a poly asset, we can delete it if the user has not placed it elsewhere
    if(static_cast<PlacementSource>(source) == PlacementSource::POLY)
    {
        auto asset_id_index = persistents.get_index<"byassetid"_n>();
        // Asset id is unique per user even if it's the same poly id
        // Otherwise it would not be clear who should pay for the RAM of a poly object
        auto itr = asset_id_index.find(source_and_asset_id);
        if(itr == asset_id_index.end())
        {
            poly_table poly(_self, _self.value);
            // Get the asset_id by unpacking the least significant bits from the composite index
            auto poly_itr = poly.find((uint64_t)source_and_asset_id);
            poly.erase(poly_itr);
        }
    }

}

void infiniverse::opendeposit(name owner)
{
    require_auth(owner);
    deposit_table deposits(_self, _self.value);
    auto deposits_itr = deposits.find(owner.value);
    if(deposits_itr == deposits.end())
    {
        deposits.emplace(owner, [&](auto &row) {
            row.owner = owner;
            row.balance = asset(0, inf_symbol);
        });
    }
}

void infiniverse::closedeposit(name owner)
{
    require_auth(owner);
    deposit_table deposits(_self, _self.value);
    auto deposits_itr = deposits.find(owner.value);
    eosio_assert(deposits_itr != deposits.end(), "User does not have a deposit opened");

    if(deposits_itr->balance.amount > 0)
    {
        transfer_inf(_self, owner, deposits_itr->balance, "");
    }

    deposits.erase(deposits_itr);
}

void infiniverse::depositinf(name from, name to, asset quantity, std::string memo)
{
    // In case the tokens are from us, or not to us, do nothing
    if(from == _self || to != _self)
        return;
    // This should never happen as we ensured transfer action belongs to "infinicoinio" account
    eosio_assert(quantity.symbol == inf_symbol, "The symbol does not match");
    eosio_assert(quantity.is_valid(), "The quantity is not valid");
    eosio_assert(quantity.amount > 0, "The amount must be positive");

    deposit_table deposits(_self, _self.value);
    auto deposits_itr = deposits.find(from.value);
    eosio_assert(deposits_itr != deposits.end(), "User does not have a deposit opened");

    deposits.modify(deposits_itr, same_payer, [&](auto &row){
        row.balance += quantity;
    });
}


uint64_t infiniverse::get_land_id_from_persistent(const persistent_table& persistents, const uint64_t& persistent_id)
{
    auto persistents_itr = persistents.find(persistent_id);
    eosio_assert(persistents_itr != persistents.end(), "Persistent Id does not exist");
    uint64_t land_id = persistents_itr->land_id;
    return land_id;
}

name infiniverse::require_land_owner_auth(const uint64_t& land_id)
{
    land_table lands(_self, _self.value);
    auto lands_itr = lands.find(land_id);
    eosio_assert(lands_itr != lands.end(), "Land Id does not exist");
    require_auth(lands_itr->owner);
    return lands_itr->owner;
}

void infiniverse::assert_vectors_within_bounds(const vector3& position,
    const vector3& orientation, const vector3& scale)
{
    eosio_assert(position.x > 0 && position.y == 0 && position.z > 0 &&
        position.x < 1 && position.z < 1,
        "Asset position is not within land bounds");

    eosio_assert(orientation.x >= 0 && orientation.x < 360 && orientation.y >= 0 &&
        orientation.y < 360 && orientation.z >= 0 && orientation.z < 360,
        "Asset orientation must be within 0 and 360");

    eosio_assert(scale.x >= 0.2 && scale.y >= 0.2 && scale.z >= 0.2, 
        "Asset scale must be at least 0.2");
}

uint64_t infiniverse::add_poly(name user, std::string poly_id)
{
    require_auth(user);

    eosio_assert((poly_id.length() == 11), "Poly Id format is invalid");

    poly_table poly(_self, _self.value);
    auto user_index = poly.get_index<"byuser"_n>();
    auto poly_itr = user_index.find(user.value);
    while(poly_itr != user_index.end() && poly_itr->user == user)
    {
        if(poly_itr->poly_id == poly_id)
        {
            return poly_itr->id;
        }
        poly_itr++;
    }

    uint64_t new_id = poly.available_primary_key();
    poly.emplace(user, [&](auto &row) {
        row.id = new_id;
        row.user = user;
        row.poly_id = poly_id;
    });
    return new_id;
}

// This function requires giving the active permission to the eosio.code permission
// cleos set account permission infiniverse1 active '{"threshold": 1,"keys": [{"key": "ACTIVE PUBKEY","weight": 1}],"accounts": [{"permission":{"actor":"infiniverse1","permission":"eosio.code"},"weight":1}]}' owner -p infiniverse1@owner
void infiniverse::transfer_inf(name from, name to, asset quantity, std::string memo)
{
    action{
        permission_level{_self, "active"_n},
        inf_account,
        "transfer"_n,
        std::tuple<name, name, asset, std::string>{from, to, quantity, memo}
    }.send();
}

extern "C" {
    void apply(uint64_t receiver, uint64_t code, uint64_t action) {
        if(code==receiver)
        {
            switch(action)
            {
                EOSIO_DISPATCH_HELPER( infiniverse, (registerland)(persistpoly)(updatepersis)(deletepersis)(opendeposit)(closedeposit) )
            }
        }
        else if(code==inf_account.value && action=="transfer"_n.value) {
            execute_action( name(receiver), name(code), &infiniverse::depositinf );
        }
    }
};