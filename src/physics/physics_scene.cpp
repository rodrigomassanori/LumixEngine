#include "physics/physics_scene.h"
#include "cooking/PxCooking.h"
#include "engine/blob.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/fs/file_system.h"
#include "engine/json_serializer.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"
#include "engine/matrix.h"
#include "engine/path.h"
#include "engine/profiler.h"
#include "engine/property_register.h"
#include "engine/resource_manager.h"
#include "engine/resource_manager_base.h"
#include "engine/universe/universe.h"
#include "lua_script/lua_script_system.h"
#include "physics/physics_geometry_manager.h"
#include "physics/physics_system.h"
#include "renderer/model.h"
#include "renderer/pose.h"
#include "renderer/render_scene.h"
#include "renderer/texture.h"
#include <PxPhysicsAPI.h>


namespace Lumix
{


static const ComponentType BOX_ACTOR_TYPE = PropertyRegister::getComponentType("box_rigid_actor");
static const ComponentType RAGDOLL_TYPE = PropertyRegister::getComponentType("ragdoll");
static const ComponentType SPHERE_ACTOR_TYPE = PropertyRegister::getComponentType("sphere_rigid_actor");
static const ComponentType CAPSULE_ACTOR_TYPE = PropertyRegister::getComponentType("capsule_rigid_actor");
static const ComponentType MESH_ACTOR_TYPE = PropertyRegister::getComponentType("mesh_rigid_actor");
static const ComponentType CONTROLLER_TYPE = PropertyRegister::getComponentType("physical_controller");
static const ComponentType HEIGHTFIELD_TYPE = PropertyRegister::getComponentType("physical_heightfield");
static const ComponentType DISTANCE_JOINT_TYPE = PropertyRegister::getComponentType("distance_joint");
static const ComponentType HINGE_JOINT_TYPE = PropertyRegister::getComponentType("hinge_joint");
static const ComponentType SPHERICAL_JOINT_TYPE = PropertyRegister::getComponentType("spherical_joint");
static const ComponentType D6_JOINT_TYPE = PropertyRegister::getComponentType("d6_joint");
static const uint32 TEXTURE_HASH = crc32("TEXTURE");
static const uint32 PHYSICS_HASH = crc32("PHYSICS");
static const uint32 RENDERER_HASH = crc32("renderer");


enum class PhysicsSceneVersion : int
{
	LAYERS,
	JOINTS,
	HINGE_JOINT,
	SPHERICAL_JOINT,
	CAPSULE_ACTOR,
	SPHERE_ACTOR,
	RAGDOLLS,
	D6_JOINT,
	JOINT_REFACTOR,

	LATEST
};


struct RagdollBone
{
	enum Type : int
	{
		BOX,
		CAPSULE
	};

	int pose_bone_idx;
	physx::PxRigidActor* actor;
	physx::PxJoint* parent_joint;
	RagdollBone* child;
	RagdollBone* next;
	RagdollBone* prev;
	RagdollBone* parent;
	Transform bind_transform;
};


struct Ragdoll
{
	Entity entity;
	RagdollBone* root;
};


struct OutputStream : public physx::PxOutputStream
{
	explicit OutputStream(IAllocator& allocator)
		: allocator(allocator)
	{
		data = (uint8*)allocator.allocate(sizeof(uint8) * 4096);
		capacity = 4096;
		size = 0;
	}

	~OutputStream() { allocator.deallocate(data); }


	virtual physx::PxU32 write(const void* src, physx::PxU32 count)
	{
		if (size + (int)count > capacity)
		{
			int new_capacity =
				Math::maximum(size + (int)count, capacity + 4096);
			uint8* new_data =
				(uint8*)allocator.allocate(sizeof(uint8) * new_capacity);
			copyMemory(new_data, data, size);
			allocator.deallocate(data);
			data = new_data;
			capacity = new_capacity;
		}
		copyMemory(data + size, src, count);
		size += count;
		return count;
	}

	uint8* data;
	IAllocator& allocator;
	int capacity;
	int size;
};


struct InputStream : public physx::PxInputStream
{
	InputStream(unsigned char* data, int size)
	{
		this->data = data;
		this->size = size;
		pos = 0;
	}

	virtual physx::PxU32 read(void* dest, physx::PxU32 count)
	{
		if (pos + (int)count <= size)
		{
			copyMemory(dest, data + pos, count);
			pos += count;
			return count;
		}
		else
		{
			copyMemory(dest, data + pos, size - pos);
			int real_count = size - pos;
			pos = size;
			return real_count;
		}
	}


	int pos;
	int size;
	unsigned char* data;
};


static Vec3 fromPhysx(const physx::PxVec3& v) { return Vec3(v.x, v.y, v.z); }
static physx::PxVec3 toPhysx(const Vec3& v) { return physx::PxVec3(v.x, v.y, v.z); }
static Quat fromPhysx(const physx::PxQuat& v) { return Quat(v.x, v.y, v.z, v.w); }
static physx::PxQuat toPhysx(const Quat& v) { return physx::PxQuat(v.x, v.y, v.z, v.w); }
static Transform fromPhysx(const physx::PxTransform& v) { return{ fromPhysx(v.p), fromPhysx(v.q) }; }
static physx::PxTransform toPhysx(const Transform& v) { return {toPhysx(v.pos), toPhysx(v.rot)}; }


struct Joint
{
	Entity connected_body;
	physx::PxJoint* physx;
	physx::PxTransform local_frame0;
};


struct Heightfield
{
	Heightfield();
	~Heightfield();
	void heightmapLoaded(Resource::State, Resource::State new_state);

	struct PhysicsSceneImpl* m_scene;
	Entity m_entity;
	physx::PxRigidActor* m_actor;
	Texture* m_heightmap;
	float m_xz_scale;
	float m_y_scale;
	int m_layer;
};


struct PhysicsSceneImpl : public PhysicsScene
{
	struct ContactCallback : public physx::PxSimulationEventCallback
	{
		explicit ContactCallback(PhysicsSceneImpl& scene)
			: m_scene(scene)
		{
		}


		void onContact(const physx::PxContactPairHeader& pairHeader,
			const physx::PxContactPair* pairs,
			physx::PxU32 nbPairs) override
		{
			for (physx::PxU32 i = 0; i < nbPairs; i++)
			{
				const auto& cp = pairs[i];

				if (!(cp.events & physx::PxPairFlag::eNOTIFY_TOUCH_FOUND)) continue;

				physx::PxContactPairPoint contact;
				auto contact_count = cp.extractContacts(&contact, 1);

				auto pos = fromPhysx(contact.position);
				Entity e1 = { (int)(intptr_t)(pairHeader.actors[0]->userData) };
				Entity e2 = { (int)(intptr_t)(pairHeader.actors[1]->userData) };
				m_scene.onContact(e1, e2, pos);
			}
		}


		void onTrigger(physx::PxTriggerPair* pairs, physx::PxU32 count) override {}
		void onConstraintBreak(physx::PxConstraintInfo*, physx::PxU32) override {}
		void onWake(physx::PxActor**, physx::PxU32) override {}
		void onSleep(physx::PxActor**, physx::PxU32) override {}


		PhysicsSceneImpl& m_scene;
	};


	class RigidActor
	{
	public:
		explicit RigidActor(PhysicsSceneImpl& _scene, ActorType _type)
			: resource(nullptr)
			, physx_actor(nullptr)
			, scene(_scene)
			, is_dynamic(false)
			, layer(0)
			, entity(INVALID_ENTITY)
			, type(_type)
		{
		}

		void setResource(PhysicsGeometry* resource);
		void setPhysxActor(physx::PxRigidActor* actor);

		Entity entity;
		int layer;
		bool is_dynamic;
		physx::PxRigidActor* physx_actor;
		PhysicsGeometry* resource;
		PhysicsSceneImpl& scene;
		ActorType type;

	private:
		void onStateChanged(Resource::State old_state, Resource::State new_state);
	};


	PhysicsSceneImpl(Universe& context, IAllocator& allocator)
		: m_allocator(allocator)
		, m_controllers(m_allocator)
		, m_actors(m_allocator)
		, m_ragdolls(m_allocator)
		, m_terrains(m_allocator)
		, m_dynamic_actors(m_allocator)
		, m_universe(context)
		, m_is_game_running(false)
		, m_contact_callback(*this)
		, m_queued_forces(m_allocator)
		, m_layers_count(2)
		, m_joints(m_allocator)
		, m_script_scene(nullptr)
		, m_debug_visualization_flags(0)
	{
		setMemory(m_layers_names, 0, sizeof(m_layers_names));
		for (int i = 0; i < lengthOf(m_layers_names); ++i)
		{
			copyString(m_layers_names[i], "Layer");
			char tmp[3];
			toCString(i, tmp, lengthOf(tmp));
			catString(m_layers_names[i], tmp);
			m_collision_filter[i] = 0xffffFFFF;
		}

		m_queued_forces.reserve(64);
	}


	~PhysicsSceneImpl()
	{
		for (int i = 0; i < m_actors.size(); ++i)
		{
			LUMIX_DELETE(m_allocator, m_actors.at(i));
		}
		for (int i = 0; i < m_terrains.size(); ++i)
		{
			LUMIX_DELETE(m_allocator, m_terrains[i]);
		}
	}


	void onContact(Entity e1, Entity e2, const Vec3& position)
	{
		if (!m_script_scene) return;

		auto send = [this](Entity e1, Entity e2, const Vec3& position)
		{
			auto cmp = m_script_scene->getComponent(e1);
			if (cmp == INVALID_COMPONENT) return;

			for (int i = 0, c = m_script_scene->getScriptCount(cmp); i < c; ++i)
			{
				auto* call = m_script_scene->beginFunctionCall(cmp, i, "onContact");
				if (!call) continue;

				call->add(e2.index);
				call->add(position.x);
				call->add(position.y);
				call->add(position.z);
				m_script_scene->endFunctionCall(*call);
			}
		};

		send(e1, e2, position);
		send(e2, e1, position);
	}


	uint32 getDebugVisualizationFlags() const override
	{
		return m_debug_visualization_flags;
	}


	void setDebugVisualizationFlags(uint32 flags) override
	{
		if (flags == m_debug_visualization_flags) return;

		m_debug_visualization_flags = flags;

		m_scene->setVisualizationParameter(physx::PxVisualizationParameter::eSCALE, flags != 0 ? 1.0f : 0.0f);

		auto setFlag = [this, flags](int flag) {
			m_scene->setVisualizationParameter(physx::PxVisualizationParameter::Enum(flag), flags & (1 << flag) ? 1.0f : 0.0f);
		};

		setFlag(physx::PxVisualizationParameter::eBODY_AXES);
		setFlag(physx::PxVisualizationParameter::eBODY_LIN_VELOCITY);
		setFlag(physx::PxVisualizationParameter::eBODY_ANG_VELOCITY);
		setFlag(physx::PxVisualizationParameter::eCONTACT_NORMAL);
		setFlag(physx::PxVisualizationParameter::eCONTACT_ERROR);
		setFlag(physx::PxVisualizationParameter::eCONTACT_FORCE);
		setFlag(physx::PxVisualizationParameter::eCOLLISION_AXES);
		setFlag(physx::PxVisualizationParameter::eJOINT_LOCAL_FRAMES);
		setFlag(physx::PxVisualizationParameter::eJOINT_LIMITS);
		setFlag(physx::PxVisualizationParameter::eCOLLISION_SHAPES);
		setFlag(physx::PxVisualizationParameter::eACTOR_AXES);
		setFlag(physx::PxVisualizationParameter::eCOLLISION_AABBS);
		setFlag(physx::PxVisualizationParameter::eWORLD_AXES);
		setFlag(physx::PxVisualizationParameter::eCONTACT_POINT);
	}


	Universe& getUniverse() override { return m_universe; }


	bool ownComponentType(ComponentType type) const override
	{
		return type == BOX_ACTOR_TYPE || type == MESH_ACTOR_TYPE || type == HEIGHTFIELD_TYPE ||
			   type == CONTROLLER_TYPE || type == DISTANCE_JOINT_TYPE || type == HINGE_JOINT_TYPE ||
			   type == SPHERICAL_JOINT_TYPE || type == CAPSULE_ACTOR_TYPE || type == SPHERE_ACTOR_TYPE ||
			   type == RAGDOLL_TYPE || type == D6_JOINT_TYPE;
	}


	ComponentHandle getComponent(Entity entity, ComponentType type) override
	{
		ASSERT(ownComponentType(type));
		if (type == BOX_ACTOR_TYPE || type == MESH_ACTOR_TYPE || type == CAPSULE_ACTOR_TYPE ||
			type == SPHERE_ACTOR_TYPE)
		{
			int idx = m_actors.find(entity);
			if (idx < 0) return INVALID_COMPONENT;
			return {entity.index};
		}
		if (type == RAGDOLL_TYPE)
		{
			int idx = m_ragdolls.find(entity);
			if (idx >= 0) return {entity.index};
			return INVALID_COMPONENT;
		}
		if (type == CONTROLLER_TYPE)
		{
			for (int i = 0; i < m_controllers.size(); ++i)
			{
				if (!m_controllers[i].m_is_free && m_controllers[i].m_entity == entity) return {i};
			}
			return INVALID_COMPONENT;
		}
		if (type == HEIGHTFIELD_TYPE)
		{
			for (int i = 0; i < m_terrains.size(); ++i)
			{
				if (m_terrains[i] && m_terrains[i]->m_entity == entity) return {i};
			}
			return INVALID_COMPONENT;
		}
		if (type == HINGE_JOINT_TYPE || type == SPHERICAL_JOINT_TYPE || type == DISTANCE_JOINT_TYPE ||
			type == D6_JOINT_TYPE)
		{
			int index = m_joints.find(entity);
			if (index < 0) return INVALID_COMPONENT;
			return {entity.index};
		}
		if (type == SPHERICAL_JOINT_TYPE)
		{
			int index = m_joints.find(entity);
			if (index < 0) return INVALID_COMPONENT;
			return {entity.index};
		}
		return INVALID_COMPONENT;
	}


	IPlugin& getPlugin() const override { return *m_system; }


	int getControllerLayer(ComponentHandle cmp) override
	{
		return m_controllers[cmp.index].m_layer;
	}


	void setControllerLayer(ComponentHandle cmp, int layer) override
	{
		ASSERT(layer < lengthOf(m_layers_names));
		m_controllers[cmp.index].m_layer = layer;

		physx::PxFilterData data;
		data.word0 = 1 << layer;
		data.word1 = m_collision_filter[layer];
		physx::PxShape* shapes[8];
		int shapes_count = m_controllers[cmp.index].m_controller->getActor()->getShapes(shapes, lengthOf(shapes));
		for (int i = 0; i < shapes_count; ++i)
		{
			shapes[i]->setSimulationFilterData(data);
		}
	}


	void setActorLayer(ComponentHandle cmp, int layer) override
	{
		ASSERT(layer < lengthOf(m_layers_names));
		auto* actor = m_actors[{cmp.index}];
		actor->layer = layer;
		updateFilterData(actor->physx_actor, actor->layer);
	}


	int getActorLayer(ComponentHandle cmp) override { return m_actors[{cmp.index}]->layer; }


	float getSphereRadius(ComponentHandle cmp) override
	{
		physx::PxRigidActor* actor = m_actors[{cmp.index}]->physx_actor;
		physx::PxShape* shapes;
		ASSERT(actor->getNbShapes() == 1);
		if (actor->getShapes(&shapes, 1) != 1) ASSERT(false);

		return shapes->getGeometry().sphere().radius;
	}


	void setSphereRadius(ComponentHandle cmp, float value) override
	{
		if (value == 0) return;
		physx::PxRigidActor* actor = m_actors[{cmp.index}]->physx_actor;
		physx::PxShape* shapes;
		if (actor->getNbShapes() == 1 && actor->getShapes(&shapes, 1))
		{
			physx::PxSphereGeometry sphere;
			bool is_sphere = shapes->getSphereGeometry(sphere);
			ASSERT(is_sphere);
			sphere.radius = value;
			shapes->setGeometry(sphere);
		}
	}


	float getCapsuleRadius(ComponentHandle cmp) override
	{
		physx::PxRigidActor* actor = m_actors[{cmp.index}]->physx_actor;
		physx::PxShape* shapes;
		ASSERT(actor->getNbShapes() == 1);
		if (actor->getShapes(&shapes, 1) != 1) ASSERT(false);

		return shapes->getGeometry().capsule().radius;
	}


	void setCapsuleRadius(ComponentHandle cmp, float value) override
	{
		if (value == 0) return;
		physx::PxRigidActor* actor = m_actors[{cmp.index}]->physx_actor;
		physx::PxShape* shapes;
		if (actor->getNbShapes() == 1 && actor->getShapes(&shapes, 1))
		{
			physx::PxCapsuleGeometry capsule;
			bool is_capsule = shapes->getCapsuleGeometry(capsule);
			ASSERT(is_capsule);
			capsule.radius = value;
			shapes->setGeometry(capsule);
		}
	}
	
	
	float getCapsuleHeight(ComponentHandle cmp) override
	{
		physx::PxRigidActor* actor = m_actors[{cmp.index}]->physx_actor;
		physx::PxShape* shapes;
		ASSERT(actor->getNbShapes() == 1);
		if (actor->getShapes(&shapes, 1) != 1) ASSERT(false);

		return shapes->getGeometry().capsule().halfHeight * 2;
	}


	void setCapsuleHeight(ComponentHandle cmp, float value) override
	{
		if (value == 0) return;
		physx::PxRigidActor* actor = m_actors[{cmp.index}]->physx_actor;
		physx::PxShape* shapes;
		if (actor->getNbShapes() == 1 && actor->getShapes(&shapes, 1))
		{
			physx::PxCapsuleGeometry capsule;
			bool is_capsule = shapes->getCapsuleGeometry(capsule);
			ASSERT(is_capsule);
			capsule.halfHeight = value * 0.5f;
			shapes->setGeometry(capsule);
		}
	}

	
	int getHeightfieldLayer(ComponentHandle cmp) override { return m_terrains[cmp.index]->m_layer; }


	void setHeightfieldLayer(ComponentHandle cmp, int layer) override
	{
		ASSERT(layer < lengthOf(m_layers_names));
		m_terrains[cmp.index]->m_layer = layer;

		if (m_terrains[cmp.index]->m_actor)
		{
			physx::PxFilterData data;
			data.word0 = 1 << layer;
			data.word1 = m_collision_filter[layer];
			physx::PxShape* shapes[8];
			int shapes_count = m_terrains[cmp.index]->m_actor->getShapes(shapes, lengthOf(shapes));
			for (int i = 0; i < shapes_count; ++i)
			{
				shapes[i]->setSimulationFilterData(data);
			}
		}
	}


	int getJointCount() override { return m_joints.size(); }
	ComponentHandle getJointComponent(int index) override { return {m_joints.getKey(index).index}; }
	Entity getJointEntity(ComponentHandle cmp) override { return {cmp.index}; }


	physx::PxDistanceJoint* getDistanceJoint(ComponentHandle cmp)
	{
		return static_cast<physx::PxDistanceJoint*>(m_joints[{cmp.index}].physx);
	}


	Vec3 getDistanceJointLinearForce(ComponentHandle cmp) override
	{
		physx::PxVec3 linear, angular;
		getDistanceJoint(cmp)->getConstraint()->getForce(linear, angular);
		return Vec3(linear.x, linear.y, linear.z);
	}


	float getDistanceJointDamping(ComponentHandle cmp) override { return getDistanceJoint(cmp)->getDamping(); }


	void setDistanceJointDamping(ComponentHandle cmp, float value) override
	{
		getDistanceJoint(cmp)->setDamping(value);
	}


	float getDistanceJointStiffness(ComponentHandle cmp) override { return getDistanceJoint(cmp)->getStiffness(); }


	void setDistanceJointStiffness(ComponentHandle cmp, float value) override
	{
		getDistanceJoint(cmp)->setStiffness(value);
	}


	float getDistanceJointTolerance(ComponentHandle cmp) override { return getDistanceJoint(cmp)->getTolerance(); }


	void setDistanceJointTolerance(ComponentHandle cmp, float value) override
	{
		getDistanceJoint(cmp)->setTolerance(value);
	}


	Vec2 getDistanceJointLimits(ComponentHandle cmp) override
	{
		auto* joint = getDistanceJoint(cmp);
		return {joint->getMinDistance(), joint->getMaxDistance()};
	}


	void setDistanceJointLimits(ComponentHandle cmp, const Vec2& value) override
	{
		auto* joint = getDistanceJoint(cmp);
		joint->setMinDistance(value.x);
		joint->setMaxDistance(value.y);
	}


	physx::PxD6Joint* getD6Joint(ComponentHandle cmp)
	{
		return static_cast<physx::PxD6Joint*>(m_joints[{cmp.index}].physx);
	}


	Vec2 getD6JointTwistLimit(ComponentHandle cmp) override
	{
		auto limit = getD6Joint(cmp)->getTwistLimit();
		return {limit.lower, limit.upper};
	}


	void setD6JointTwistLimit(ComponentHandle cmp, const Vec2& limit) override
	{
		auto* joint = getD6Joint(cmp);
		auto px_limit = joint->getTwistLimit();
		px_limit.lower = limit.x;
		px_limit.upper = limit.y;
		joint->setTwistLimit(px_limit);
	}


	Vec2 getD6JointSwingLimit(ComponentHandle cmp) override
	{
		auto limit = getD6Joint(cmp)->getSwingLimit();
		return {limit.yAngle, limit.zAngle};
	}


	void setD6JointSwingLimit(ComponentHandle cmp, const Vec2& limit)
	{
		auto* joint = getD6Joint(cmp);
		auto px_limit = joint->getSwingLimit();
		px_limit.yAngle = limit.x;
		px_limit.zAngle = limit.y;
		joint->setSwingLimit(px_limit);
	}


	physx::PxD6Motion::Enum getD6JointXMotion(ComponentHandle cmp) override
	{
		return getD6Joint(cmp)->getMotion(physx::PxD6Axis::eX);
	}


	void setD6JointXMotion(ComponentHandle cmp, physx::PxD6Motion::Enum motion) override
	{
		getD6Joint(cmp)->setMotion(physx::PxD6Axis::eX, (physx::PxD6Motion::Enum)motion);
	}


	physx::PxD6Motion::Enum getD6JointYMotion(ComponentHandle cmp) override
	{
		return getD6Joint(cmp)->getMotion(physx::PxD6Axis::eY);
	}


	void setD6JointYMotion(ComponentHandle cmp, physx::PxD6Motion::Enum motion) override
	{
		getD6Joint(cmp)->setMotion(physx::PxD6Axis::eY, (physx::PxD6Motion::Enum)motion);
	}


	physx::PxD6Motion::Enum getD6JointSwing1Motion(ComponentHandle cmp) override
	{
		return getD6Joint(cmp)->getMotion(physx::PxD6Axis::eSWING1);
	}


	void setD6JointSwing1Motion(ComponentHandle cmp, physx::PxD6Motion::Enum motion) override
	{
		getD6Joint(cmp)->setMotion(physx::PxD6Axis::eSWING1, (physx::PxD6Motion::Enum)motion);
	}


	physx::PxD6Motion::Enum getD6JointSwing2Motion(ComponentHandle cmp) override
	{
		return getD6Joint(cmp)->getMotion(physx::PxD6Axis::eSWING2);
	}


	void setD6JointSwing2Motion(ComponentHandle cmp, physx::PxD6Motion::Enum motion) override
	{
		getD6Joint(cmp)->setMotion(physx::PxD6Axis::eSWING2, (physx::PxD6Motion::Enum)motion);
	}


	physx::PxD6Motion::Enum getD6JointTwistMotion(ComponentHandle cmp) override
	{
		return getD6Joint(cmp)->getMotion(physx::PxD6Axis::eTWIST);
	}


	void setD6JointTwistMotion(ComponentHandle cmp, physx::PxD6Motion::Enum motion) override
	{
		getD6Joint(cmp)->setMotion(physx::PxD6Axis::eTWIST, (physx::PxD6Motion::Enum)motion);
	}


	physx::PxD6Motion::Enum getD6JointZMotion(ComponentHandle cmp) override
	{
		return getD6Joint(cmp)->getMotion(physx::PxD6Axis::eZ);
	}


	void setD6JointZMotion(ComponentHandle cmp, physx::PxD6Motion::Enum motion) override
	{
		getD6Joint(cmp)->setMotion(physx::PxD6Axis::eZ, (physx::PxD6Motion::Enum)motion);
	}


	float getD6JointLinearLimit(ComponentHandle cmp) override
	{
		return getD6Joint(cmp)->getLinearLimit().value;
	}


	void setD6JointLinearLimit(ComponentHandle cmp, float limit) override
	{
		auto* joint = getD6Joint(cmp);
		auto px_limit = joint->getLinearLimit();
		px_limit.value = limit;
		joint->setLinearLimit(px_limit);
	}


	Entity getJointConnectedBody(ComponentHandle cmp) override
	{
		return m_joints[{cmp.index}].connected_body;
	}


	void setJointConnectedBody(ComponentHandle cmp, Entity entity) override
	{
		m_joints[{cmp.index}].connected_body = entity;
	}


	void setJointAxisPosition(ComponentHandle cmp, const Vec3& value) override
	{
		auto& joint = m_joints[{cmp.index}];
		joint.local_frame0.p = toPhysx(value);
		joint.physx->setLocalPose(physx::PxJointActorIndex::eACTOR0, joint.local_frame0);
	}


	void setJointAxisDirection(ComponentHandle cmp, const Vec3& value) override
	{
		auto& joint = m_joints[{cmp.index}];
		joint.local_frame0.q = toPhysx(Quat::vec3ToVec3(Vec3(1, 0, 0), value));
		joint.physx->setLocalPose(physx::PxJointActorIndex::eACTOR0, joint.local_frame0);
	}


	Vec3 getJointAxisPosition(ComponentHandle cmp) override
	{
		return fromPhysx(m_joints[{cmp.index}].local_frame0.p);
	}


	Vec3 getJointAxisDirection(ComponentHandle cmp) override
	{
		return fromPhysx(m_joints[{cmp.index}].local_frame0.q.rotate(physx::PxVec3(1, 0, 0)));
	}


	bool getSphericalJointUseLimit(ComponentHandle cmp) override
	{
		return static_cast<physx::PxSphericalJoint*>(m_joints[{cmp.index}].physx)
			->getSphericalJointFlags()
			.isSet(physx::PxSphericalJointFlag::eLIMIT_ENABLED);
	}


	void setSphericalJointUseLimit(ComponentHandle cmp, bool use_limit) override
	{
		return static_cast<physx::PxSphericalJoint*>(m_joints[{cmp.index}].physx)
			->setSphericalJointFlag(physx::PxSphericalJointFlag::eLIMIT_ENABLED, use_limit);
	}


	Vec2 getSphericalJointLimit(ComponentHandle cmp) override
	{
		auto cone = static_cast<physx::PxSphericalJoint*>(m_joints[{cmp.index}].physx)->getLimitCone();
		return {cone.yAngle, cone.zAngle};
	}


	void setSphericalJointLimit(ComponentHandle cmp, const Vec2& limit) override
	{
		auto* joint = static_cast<physx::PxSphericalJoint*>(m_joints[{cmp.index}].physx);
		auto limit_cone = joint->getLimitCone();
		limit_cone.yAngle = limit.x;
		limit_cone.zAngle = limit.y;
		joint->setLimitCone(limit_cone);
	}


	Transform getJointLocalFrame(ComponentHandle cmp) override
	{
		return fromPhysx(m_joints[{cmp.index}].local_frame0);
	}


	physx::PxJoint* getJoint(ComponentHandle cmp) override
	{
		return m_joints[{cmp.index}].physx;
	}


	Transform getJointConnectedBodyLocalFrame(ComponentHandle cmp) override
	{
		auto& joint = m_joints[{cmp.index}];
		if (!isValid(joint.connected_body)) return {Vec3(0, 0, 0), Quat(0, 0, 0, 1)};
		
		physx::PxRigidActor* a0, *a1;
		joint.physx->getActors(a0, a1);
		if (a1) return fromPhysx(joint.physx->getLocalPose(physx::PxJointActorIndex::eACTOR1));

		Transform tr = m_universe.getTransform(joint.connected_body);
		return tr.inverted() * m_universe.getTransform({cmp.index}) * fromPhysx(joint.local_frame0);
	}


	void setHingeJointUseLimit(ComponentHandle cmp, bool use_limit) override
	{
		auto* joint = static_cast<physx::PxRevoluteJoint*>(m_joints[{cmp.index}].physx);
		joint->setRevoluteJointFlag(physx::PxRevoluteJointFlag::eLIMIT_ENABLED, use_limit);
	}


	bool getHingeJointUseLimit(ComponentHandle cmp) override
	{
		auto* joint = static_cast<physx::PxRevoluteJoint*>(m_joints[{cmp.index}].physx);
		return joint->getRevoluteJointFlags().isSet(physx::PxRevoluteJointFlag::eLIMIT_ENABLED);
	}


	Vec2 getHingeJointLimit(ComponentHandle cmp) override
	{
		auto* joint = static_cast<physx::PxRevoluteJoint*>(m_joints[{cmp.index}].physx);
		physx::PxJointAngularLimitPair limit = joint->getLimit();
		return {limit.lower, limit.upper};
	}


	void setHingeJointLimit(ComponentHandle cmp, const Vec2& limit) override
	{
		auto* joint = static_cast<physx::PxRevoluteJoint*>(m_joints[{cmp.index}].physx);
		physx::PxJointAngularLimitPair px_limit = joint->getLimit();
		px_limit.lower = limit.x;
		px_limit.upper = limit.y;
		joint->setLimit(px_limit);
	}


	float getHingeJointDamping(ComponentHandle cmp) override
	{
		auto* joint = static_cast<physx::PxRevoluteJoint*>(m_joints[{cmp.index}].physx);
		return joint->getLimit().damping;
	}


	void setHingeJointDamping(ComponentHandle cmp, float value) override
	{
		auto* joint = static_cast<physx::PxRevoluteJoint*>(m_joints[{cmp.index}].physx);
		physx::PxJointAngularLimitPair px_limit = joint->getLimit();
		px_limit.damping = value;
		joint->setLimit(px_limit);

	}


	float getHingeJointStiffness(ComponentHandle cmp) override
	{
		auto* joint = static_cast<physx::PxRevoluteJoint*>(m_joints[{cmp.index}].physx);
		return joint->getLimit().stiffness;
	}


	void setHingeJointStiffness(ComponentHandle cmp, float value) override
	{
		auto* joint = static_cast<physx::PxRevoluteJoint*>(m_joints[{cmp.index}].physx);
		physx::PxJointAngularLimitPair px_limit = joint->getLimit();
		px_limit.stiffness = value;
		joint->setLimit(px_limit);
	}


	ComponentHandle createComponent(ComponentType component_type, Entity entity) override
	{
		if (component_type == DISTANCE_JOINT_TYPE)
		{
			return createDistanceJoint(entity);
		}
		else if (component_type == HINGE_JOINT_TYPE)
		{
			return createHingeJoint(entity);
		}
		else if (component_type == SPHERICAL_JOINT_TYPE)
		{
			return createSphericalJoint(entity);
		}
		else if (component_type == D6_JOINT_TYPE)
		{
			return createD6Joint(entity);
		}
		else if (component_type == HEIGHTFIELD_TYPE)
		{
			return createHeightfield(entity);
		}
		else if (component_type == CONTROLLER_TYPE)
		{
			return createController(entity);
		}
		else if (component_type == BOX_ACTOR_TYPE)
		{
			return createBoxRigidActor(entity);
		}
		else if (component_type == RAGDOLL_TYPE)
		{
			return createRagdoll(entity);
		}
		else if (component_type == SPHERE_ACTOR_TYPE)
		{
			return createSphereRigidActor(entity);
		}
		else if (component_type == CAPSULE_ACTOR_TYPE)
		{
			return createCapsuleRigidActor(entity);
		}
		else if (component_type == MESH_ACTOR_TYPE)
		{
			return createMeshRigidActor(entity);
		}
		return INVALID_COMPONENT;
	}


	void destroyComponent(ComponentHandle cmp, ComponentType type) override
	{
		if (type == HEIGHTFIELD_TYPE)
		{
			Entity entity = m_terrains[cmp.index]->m_entity;
			LUMIX_DELETE(m_allocator, m_terrains[cmp.index]);
			m_terrains[cmp.index] = nullptr;
			m_universe.destroyComponent(entity, type, this, cmp);
		}
		else if (type == CONTROLLER_TYPE)
		{
			Entity entity = m_controllers[cmp.index].m_entity;
			m_controllers[cmp.index].m_is_free = true;
			m_universe.destroyComponent(entity, type, this, cmp);
		}
		else if (type == MESH_ACTOR_TYPE || type == BOX_ACTOR_TYPE || type == CAPSULE_ACTOR_TYPE ||
				 type == SPHERE_ACTOR_TYPE)
		{
			Entity entity = {cmp.index};
			auto* actor = m_actors[entity];
			actor->setPhysxActor(nullptr);
			LUMIX_DELETE(m_allocator, actor);
			m_actors.erase(entity);
			m_dynamic_actors.eraseItem(actor);
			m_universe.destroyComponent(entity, type, this, cmp);
		}
		else if (type == RAGDOLL_TYPE)
		{
			int idx = m_ragdolls.find({cmp.index});
			Entity entity = m_ragdolls.at(idx).entity;
			destroySkeleton(m_ragdolls.at(idx).root);
			m_ragdolls.eraseAt(idx);
			m_universe.destroyComponent(entity, type, this, cmp);
		}
		else if (type == SPHERICAL_JOINT_TYPE || type == HINGE_JOINT_TYPE || type == DISTANCE_JOINT_TYPE ||
				 type == D6_JOINT_TYPE)
		{
			Entity entity = {cmp.index};
			auto& joint = m_joints[entity];
			if (joint.physx) joint.physx->release();
			m_joints.erase(entity);
			m_universe.destroyComponent(entity, type, this, cmp);
		}
		else
		{
			ASSERT(false);
		}
	}


	ComponentHandle createDistanceJoint(Entity entity)
	{
		Joint joint;
		joint.connected_body = INVALID_ENTITY;
		joint.local_frame0.p = physx::PxVec3(0, 0, 0);
		joint.local_frame0.q = physx::PxQuat(0, 0, 0, 1);
		joint.physx = physx::PxDistanceJointCreate(m_scene->getPhysics(),
			m_dummy_actor,
			physx::PxTransform::createIdentity(),
			nullptr,
			physx::PxTransform::createIdentity());
		joint.physx->setConstraintFlag(physx::PxConstraintFlag::eVISUALIZATION, true);
		m_joints.insert(entity, joint);

		ComponentHandle cmp = {entity.index};
		m_universe.addComponent(entity, DISTANCE_JOINT_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createSphericalJoint(Entity entity)
	{
		Joint joint;
		joint.connected_body = INVALID_ENTITY;
		joint.local_frame0.p = physx::PxVec3(0, 0, 0);
		joint.local_frame0.q = physx::PxQuat(0, 0, 0, 1);
		joint.physx = physx::PxSphericalJointCreate(m_scene->getPhysics(),
			m_dummy_actor,
			physx::PxTransform::createIdentity(),
			nullptr,
			physx::PxTransform::createIdentity());
		joint.physx->setConstraintFlag(physx::PxConstraintFlag::eVISUALIZATION, true);
		m_joints.insert(entity, joint);

		ComponentHandle cmp = {entity.index};
		m_universe.addComponent(entity, SPHERICAL_JOINT_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createD6Joint(Entity entity)
	{
		Joint joint;
		joint.connected_body = INVALID_ENTITY;
		joint.local_frame0.p = physx::PxVec3(0, 0, 0);
		joint.local_frame0.q = physx::PxQuat(0, 0, 0, 1);
		joint.physx = physx::PxD6JointCreate(m_scene->getPhysics(),
			m_dummy_actor,
			physx::PxTransform::createIdentity(),
			nullptr,
			physx::PxTransform::createIdentity());
		auto* d6_joint = static_cast<physx::PxD6Joint*>(joint.physx);
		auto linear_limit = d6_joint->getLinearLimit();
		linear_limit.value = 1.0f;
		d6_joint->setLinearLimit(linear_limit);
		joint.physx->setConstraintFlag(physx::PxConstraintFlag::eVISUALIZATION, true);
		m_joints.insert(entity, joint);

		ComponentHandle cmp = {entity.index};
		m_universe.addComponent(entity, D6_JOINT_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createHingeJoint(Entity entity)
	{
		Joint joint;
		joint.connected_body = INVALID_ENTITY;
		joint.local_frame0.p = physx::PxVec3(0, 0, 0);
		joint.local_frame0.q = physx::PxQuat(0, 0, 0, 1);
		joint.physx = physx::PxRevoluteJointCreate(m_scene->getPhysics(),
			m_dummy_actor,
			physx::PxTransform::createIdentity(),
			nullptr,
			physx::PxTransform::createIdentity());
		joint.physx->setConstraintFlag(physx::PxConstraintFlag::eVISUALIZATION, true);
		m_joints.insert(entity, joint);

		ComponentHandle cmp = { entity.index };
		m_universe.addComponent(entity, HINGE_JOINT_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createHeightfield(Entity entity)
	{
		Heightfield* terrain = LUMIX_NEW(m_allocator, Heightfield)();
		m_terrains.push(terrain);
		terrain->m_heightmap = nullptr;
		terrain->m_scene = this;
		terrain->m_actor = nullptr;
		terrain->m_entity = entity;
		ComponentHandle cmp = {m_terrains.size() - 1};
		m_universe.addComponent(entity, HEIGHTFIELD_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createController(Entity entity)
	{
		physx::PxCapsuleControllerDesc cDesc;
		cDesc.material = m_default_material;
		cDesc.height = 1.8f;
		cDesc.radius = 0.25f;
		cDesc.slopeLimit = 0.0f;
		cDesc.contactOffset = 0.1f;
		cDesc.stepOffset = 0.02f;
		cDesc.callback = nullptr;
		cDesc.behaviorCallback = nullptr;
		Vec3 position = m_universe.getPosition(entity);
		cDesc.position.set(position.x, position.y, position.z);
		PhysicsSceneImpl::Controller& c = m_controllers.emplace();
		c.m_controller = m_controller_manager->createController(cDesc);
		c.m_entity = entity;
		c.m_is_free = false;
		c.m_frame_change.set(0, 0, 0);
		c.m_radius = cDesc.radius;
		c.m_height = cDesc.height;
		c.m_layer = 0;

		physx::PxFilterData data;
		int controller_layer = c.m_layer;
		data.word0 = 1 << controller_layer;
		data.word1 = m_collision_filter[controller_layer];
		physx::PxShape* shapes[8];
		int shapes_count = c.m_controller->getActor()->getShapes(shapes, lengthOf(shapes));
		for (int i = 0; i < shapes_count; ++i)
		{
			shapes[i]->setSimulationFilterData(data);
		}

		ComponentHandle cmp = {m_controllers.size() - 1};
		m_universe.addComponent(entity, CONTROLLER_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createCapsuleRigidActor(Entity entity)
	{
		RigidActor* actor = LUMIX_NEW(m_allocator, RigidActor)(*this, ActorType::CAPSULE);
		m_actors.insert(entity, actor);
		actor->entity = entity;

		physx::PxCapsuleGeometry geom;
		geom.radius = 0.5f;
		geom.halfHeight = 1;
		Transform transform = m_universe.getTransform(entity);
		physx::PxTransform px_transform = toPhysx(transform);

		physx::PxRigidStatic* physx_actor =
			PxCreateStatic(*m_system->getPhysics(), px_transform, geom, *m_default_material);
		actor->setPhysxActor(physx_actor);

		ComponentHandle cmp = {m_actors.size() - 1};
		m_universe.addComponent(entity, CAPSULE_ACTOR_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createRagdoll(Entity entity)
	{
		int idx = m_ragdolls.insert(entity, Ragdoll());
		Ragdoll& ragdoll = m_ragdolls.at(idx);
		ragdoll.entity = entity;
		ragdoll.root = nullptr;

		ComponentHandle cmp = {entity.index};
		m_universe.addComponent(entity, RAGDOLL_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createBoxRigidActor(Entity entity)
	{
		RigidActor* actor = LUMIX_NEW(m_allocator, RigidActor)(*this, ActorType::BOX);
		m_actors.insert(entity, actor);
		actor->entity = entity;

		physx::PxBoxGeometry geom;
		geom.halfExtents.x = 1;
		geom.halfExtents.y = 1;
		geom.halfExtents.z = 1;
		Transform transform = m_universe.getTransform(entity);
		physx::PxTransform px_transform = toPhysx(transform);

		physx::PxRigidStatic* physx_actor =
			PxCreateStatic(*m_system->getPhysics(), px_transform, geom, *m_default_material);
		actor->setPhysxActor(physx_actor);

		ComponentHandle cmp = {m_actors.size() - 1};
		m_universe.addComponent(entity, BOX_ACTOR_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createSphereRigidActor(Entity entity)
	{
		RigidActor* actor = LUMIX_NEW(m_allocator, RigidActor)(*this, ActorType::SPHERE);
		m_actors.insert(entity, actor);
		actor->entity = entity;

		physx::PxSphereGeometry geom;
		geom.radius = 1;
		physx::PxTransform transform = toPhysx(m_universe.getTransform(entity));

		physx::PxRigidStatic* physx_actor =
			PxCreateStatic(*m_system->getPhysics(), transform, geom, *m_default_material);
		actor->setPhysxActor(physx_actor);

		ComponentHandle cmp = {m_actors.size() - 1};
		m_universe.addComponent(entity, SPHERE_ACTOR_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createMeshRigidActor(Entity entity)
	{
		RigidActor* actor = LUMIX_NEW(m_allocator, RigidActor)(*this, ActorType::MESH);
		m_actors.insert(entity, actor);
		actor->entity = entity;

		ComponentHandle cmp = {m_actors.size() - 1};
		m_universe.addComponent(entity, MESH_ACTOR_TYPE, this, cmp);
		return cmp;
	}


	Path getHeightmap(ComponentHandle cmp) override
	{
		return m_terrains[cmp.index]->m_heightmap ? m_terrains[cmp.index]->m_heightmap->getPath() : Path("");
	}


	float getHeightmapXZScale(ComponentHandle cmp) override { return m_terrains[cmp.index]->m_xz_scale; }


	void setHeightmapXZScale(ComponentHandle cmp, float scale) override
	{
		auto* terrain = m_terrains[cmp.index];
		if (scale != terrain->m_xz_scale)
		{
			terrain->m_xz_scale = scale;
			if (terrain->m_heightmap && terrain->m_heightmap->isReady())
			{
				heightmapLoaded(terrain);
			}
		}
	}


	float getHeightmapYScale(ComponentHandle cmp) override
	{
		return m_terrains[cmp.index]->m_y_scale;
	}


	void setHeightmapYScale(ComponentHandle cmp, float scale) override
	{
		auto* terrain = m_terrains[cmp.index];
		if (scale != terrain->m_y_scale)
		{
			terrain->m_y_scale = scale;
			if (terrain->m_heightmap && terrain->m_heightmap->isReady())
			{
				heightmapLoaded(terrain);
			}
		}
	}


	void setHeightmap(ComponentHandle cmp, const Path& str) override
	{
		auto& resource_manager = m_engine->getResourceManager();
		auto* terrain = m_terrains[cmp.index];
		auto* old_hm = terrain->m_heightmap;
		if (old_hm)
		{
			resource_manager.get(TEXTURE_HASH)->unload(*old_hm);
			auto& cb = old_hm->getObserverCb();
			cb.unbind<Heightfield, &Heightfield::heightmapLoaded>(terrain);
		}
		auto* texture_manager = resource_manager.get(TEXTURE_HASH);
		if (str.isValid())
		{
			auto* new_hm = static_cast<Texture*>(texture_manager->load(str));
			terrain->m_heightmap = new_hm;
			new_hm->onLoaded<Heightfield, &Heightfield::heightmapLoaded>(terrain);
			new_hm->addDataReference();
		}
		else
		{
			terrain->m_heightmap = nullptr;
		}
	}


	Path getShapeSource(ComponentHandle cmp) override
	{
		return m_actors[{cmp.index}]->resource ? m_actors[{cmp.index}]->resource->getPath() : Path("");
	}


	void setShapeSource(ComponentHandle cmp, const Path& str) override
	{
		ASSERT(m_actors[{cmp.index}]);
		bool is_dynamic = isDynamic(cmp);
		auto& actor = *m_actors[{cmp.index}];
		if (actor.resource && actor.resource->getPath() == str &&
			(!actor.physx_actor || is_dynamic == !actor.physx_actor->isRigidStatic()))
		{
			return;
		}

		ResourceManagerBase* manager = m_engine->getResourceManager().get(PHYSICS_HASH);
		PhysicsGeometry* geom_res = static_cast<PhysicsGeometry*>(manager->load(str));

		actor.setPhysxActor(nullptr);
		actor.setResource(geom_res);
	}


	void setControllerPosition(int index, const Vec3& pos)
	{
		physx::PxExtendedVec3 p(pos.x, pos.y, pos.z);
		m_controllers[index].m_controller->setPosition(p);
	}


	int getActorCount() const override { return m_actors.size(); }
	Entity getActorEntity(int index) override { return m_actors.at(index)->entity; }
	ActorType getActorType(int index) override { return m_actors.at(index)->type; }
	ComponentHandle getActorComponentHandle(int index) override { return {index}; }


	bool isActorDebugEnabled(int index) const override
	{
		auto* px_actor = m_actors.at(index)->physx_actor;
		if (!px_actor) return false;
		return px_actor->getActorFlags().isSet(physx::PxActorFlag::eVISUALIZATION);
	}


	void enableActorDebug(int index, bool enable) const override
	{
		auto* px_actor = m_actors.at(index)->physx_actor;
		if (!px_actor) return;
		px_actor->setActorFlag(physx::PxActorFlag::eVISUALIZATION, enable);
		physx::PxShape* shape;
		int count = px_actor->getShapes(&shape, 1);
		ASSERT(count > 0);
		shape->setFlag(physx::PxShapeFlag::eVISUALIZATION, enable);
	}


	void render() override
	{
		auto& render_scene = *static_cast<RenderScene*>(m_universe.getScene(crc32("renderer")));
		const physx::PxRenderBuffer& rb = m_scene->getRenderBuffer();
		const physx::PxU32 num_lines = rb.getNbLines();
		if (num_lines)
		{
			const physx::PxDebugLine* PX_RESTRICT lines = rb.getLines();
			for (physx::PxU32 i = 0; i < num_lines; ++i)
			{
				const physx::PxDebugLine& line = lines[i];
				Vec3 from = fromPhysx(line.pos0);
				Vec3 to = fromPhysx(line.pos1);
				render_scene.addDebugLine(from, to, line.color0, 0);
			}
		}
		const physx::PxU32 num_tris = rb.getNbTriangles();
		if (num_tris)
		{
			const physx::PxDebugTriangle* PX_RESTRICT tris = rb.getTriangles();
			for (physx::PxU32 i = 0; i < num_tris; ++i)
			{
				const physx::PxDebugTriangle& tri = tris[i];
				render_scene.addDebugTriangle(fromPhysx(tri.pos0), fromPhysx(tri.pos1), fromPhysx(tri.pos2), tri.color0, 0);
			}
		}
	}


	void updateDynamicActors()
	{
		PROFILE_FUNCTION();
		for (auto* actor : m_dynamic_actors)
		{
			physx::PxTransform trans = actor->physx_actor->getGlobalPose();
			m_universe.setPosition(actor->entity, trans.p.x, trans.p.y, trans.p.z);
			m_universe.setRotation(actor->entity, trans.q.x, trans.q.y, trans.q.z, trans.q.w);
		}
	}


	void simulateScene(float time_delta)
	{
		PROFILE_FUNCTION();
		m_scene->simulate(time_delta);
	}


	void fetchResults()
	{
		PROFILE_FUNCTION();
		m_scene->fetchResults(true);
	}


	void updateControllers(float time_delta)
	{
		PROFILE_FUNCTION();
		Vec3 g(0, time_delta * -9.8f, 0);
		for (int i = 0; i < m_controllers.size(); ++i)
		{
			if (m_controllers[i].m_is_free) continue;

			Vec3 dif = g + m_controllers[i].m_frame_change;
			m_controllers[i].m_frame_change.set(0, 0, 0);
			const physx::PxExtendedVec3& p = m_controllers[i].m_controller->getPosition();
			m_controllers[i].m_controller->move(
				physx::PxVec3(dif.x, dif.y, dif.z), 0.01f, time_delta, physx::PxControllerFilters());

			float y = (float)p.y - m_controllers[i].m_height * 0.5f - m_controllers[i].m_radius;
			m_universe.setPosition(m_controllers[i].m_entity, (float)p.x, y, (float)p.z);
		}
	}


	void applyQueuedForces()
	{
		for (auto& i : m_queued_forces)
		{
			auto* actor = m_actors[{i.cmp.index}];
			if (!actor->is_dynamic)
			{
				g_log_warning.log("Physics") << "Trying to apply force to static object";
				return;
			}

			auto* physx_actor = static_cast<physx::PxRigidDynamic*>(actor->physx_actor);
			if (!physx_actor) return;
			physx::PxVec3 f(i.force.x, i.force.y, i.force.z);
			physx_actor->addForce(f);
		}
		m_queued_forces.clear();
	}


	static RagdollBoneHandle getBone(RagdollBone* bone, int pose_bone_idx)
	{
		if (!bone) return nullptr;
		if (bone->pose_bone_idx == pose_bone_idx) return bone;

		auto* handle = getBone(bone->child, pose_bone_idx);
		if (handle) return handle;
			
		handle = getBone(bone->next, pose_bone_idx);
		if (handle) return handle;

		return nullptr;
	}


	physx::PxCapsuleGeometry getCapsuleGeometry(RagdollBone* bone)
	{
		physx::PxShape* shape;
		int count = bone->actor->getShapes(&shape, 1);
		ASSERT(count == 1);

		physx::PxCapsuleGeometry geom;
		bool is_capsule = shape->getCapsuleGeometry(geom);
		ASSERT(is_capsule);

		return geom;
	}


	physx::PxJoint* getRagdollBoneJoint(RagdollBoneHandle bone) const override
	{
		return bone->parent_joint;
	}


	RagdollBoneHandle getRagdollRootBone(ComponentHandle cmp) const override
	{
		return m_ragdolls[{cmp.index}].root;
	}


	RagdollBoneHandle getRagdollBoneChild(RagdollBoneHandle bone) override
	{
		return bone->child;
	}


	RagdollBoneHandle getRagdollBoneSibling(RagdollBoneHandle bone) override
	{
		return bone->next;
	}


	float getRagdollBoneHeight(RagdollBoneHandle bone) override
	{
		return getCapsuleGeometry(bone).halfHeight * 2.0f;
	}


	float getRagdollBoneRadius(RagdollBoneHandle bone) override
	{
		return getCapsuleGeometry(bone).radius;
	}


	void setRagdollBoneHeight(RagdollBoneHandle bone, float value) override
	{
		if (value < 0) return;
		auto geom = getCapsuleGeometry(bone);
		geom.halfHeight = value * 0.5f;
		physx::PxShape* shape;
		bone->actor->getShapes(&shape, 1);
		shape->setGeometry(geom);
	}


	void setRagdollBoneRadius(RagdollBoneHandle bone, float value) override
	{
		if (value < 0) return;
		auto geom = getCapsuleGeometry(bone);
		geom.radius = value;
		physx::PxShape* shape;
		bone->actor->getShapes(&shape, 1);
		shape->setGeometry(geom);
	}


	Transform getRagdollBoneTransform(RagdollBoneHandle bone) override
	{
		auto px_pose = bone->actor->getGlobalPose();
		return {fromPhysx(px_pose.p), fromPhysx(px_pose.q)};
	}


	void setRagdollBoneTransform(RagdollBoneHandle bone, const Transform& transform) override
	{
		bone->actor->setGlobalPose(toPhysx(transform));
	}


	RagdollBoneHandle getRagdollBoneByName(ComponentHandle cmp, uint32 bone_name_hash) override
	{
		Entity entity = {cmp.index};
		auto* render_scene = static_cast<RenderScene*>(m_universe.getScene(RENDERER_HASH));
		ASSERT(render_scene);

		ComponentHandle renderable = render_scene->getRenderableComponent(entity);
		ASSERT(isValid(renderable));
		Model* model = render_scene->getRenderableModel(renderable);
		ASSERT(model && model->isReady());

		auto iter = model->getBoneIndex(bone_name_hash);
		ASSERT(iter.isValid());

		return getBone(m_ragdolls[entity].root, iter.value());
	}


	RagdollBone* getPhyParent(ComponentHandle cmp, Model* model, int bone_index)
	{
		auto* bone = &model->getBone(bone_index);
		if (bone->parent_idx < 0) return nullptr;
		RagdollBone* phy_bone = nullptr;
		do
		{
			bone = &model->getBone(bone->parent_idx);
			phy_bone = getRagdollBoneByName(cmp, crc32(bone->name.c_str()));
		} while (!phy_bone && bone->parent_idx >= 0);
		return phy_bone;
	}


	void destroyRagdollBone(ComponentHandle cmp, RagdollBoneHandle bone) override
	{
		disconnect(m_ragdolls[{cmp.index}], bone);
		bone->actor->release();
		LUMIX_DELETE(m_allocator, bone);
	}


	void changeRagdollBoneJoint(RagdollBone* child, int type) override
	{
		if (child->parent_joint) child->parent_joint->release();

		physx::PxJointConcreteType::Enum px_type = (physx::PxJointConcreteType::Enum)type;
		auto d1 = child->actor->getGlobalPose().q.rotate(physx::PxVec3(1, 0, 0));
		auto d2 = child->parent->actor->getGlobalPose().q.rotate(physx::PxVec3(1, 0, 0));
		auto axis = d1.cross(d2).getNormalized();
		auto pos = child->parent->actor->getGlobalPose().p;
		physx::PxMat44 mat(d1, axis, d1.cross(axis).getNormalized(), pos);
		physx::PxTransform tr0 = child->parent->actor->getGlobalPose().getInverse() * physx::PxTransform(mat);
		physx::PxTransform tr1 = child->actor->getGlobalPose().getInverse() * child->parent->actor->getGlobalPose() * tr0;

		physx::PxJoint* joint = nullptr;
		switch (px_type)
		{
			case physx::PxJointConcreteType::eFIXED:
				joint = physx::PxFixedJointCreate(m_scene->getPhysics(), child->parent->actor, tr0, child->actor, tr1);
				break;
			case physx::PxJointConcreteType::eREVOLUTE:
				joint =
					physx::PxRevoluteJointCreate(m_scene->getPhysics(), child->parent->actor, tr0, child->actor, tr1);
				if (joint) ((physx::PxRevoluteJoint*)joint)->setProjectionLinearTolerance(0.1f);
				break;
			case physx::PxJointConcreteType::eSPHERICAL:
				joint =
					physx::PxSphericalJointCreate(m_scene->getPhysics(), child->parent->actor, tr0, child->actor, tr1);
				if (joint) ((physx::PxSphericalJoint*)joint)->setProjectionLinearTolerance(0.1f);
				break;
			default: ASSERT(false); break;
		}

		if (joint)
		{
			joint->setConstraintFlag(physx::PxConstraintFlag::eVISUALIZATION, true);
			joint->setConstraintFlag(physx::PxConstraintFlag::eCOLLISION_ENABLED, false);
			joint->setConstraintFlag(physx::PxConstraintFlag::ePROJECTION, true);
		}
		child->parent_joint = joint;
	}


	void disconnect(Ragdoll& ragdoll, RagdollBone* bone)
	{
		auto* child = bone->child;
		auto* parent = bone->parent;
		if (parent && parent->child == bone) parent->child = bone->next;
		if (ragdoll.root == bone) ragdoll.root = bone->next;
		if (bone->prev) bone->prev->next = bone->next;
		if (bone->next) bone->next->prev = bone->prev;

		while (child)
		{
			auto* next = child->next;

			if (child->parent_joint) child->parent_joint->release();
			child->parent_joint = nullptr;

			if (parent)
			{
				child->next = parent->child;
				child->prev = nullptr;
				if (child->next) child->next->prev = child;
				parent->child = child;
				child->parent = parent;
				changeRagdollBoneJoint(child, physx::PxJointConcreteType::eREVOLUTE);
			}
			else
			{
				child->parent = nullptr;
				child->next = ragdoll.root;
				child->prev = nullptr;
				if (child->next) child->next->prev = child;
				ragdoll.root = child;
			}
			child = next;
		}
		if (bone->parent_joint) bone->parent_joint->release();
		bone->parent_joint = nullptr;

		bone->parent = nullptr;
		bone->child = nullptr;
		bone->prev = nullptr;
		bone->next = ragdoll.root;
		if(bone->next) bone->next->prev = bone;
	}


	void connect(Ragdoll& ragdoll, RagdollBone* child, RagdollBone* parent)
	{
		ASSERT(!child->parent);
		ASSERT(!child->child);
		if (child->next) child->next->prev = child->prev;
		if (child->prev) child->prev->next = child->next;
		if (ragdoll.root == child) ragdoll.root = child->next;
		child->next = parent->child;
		if (child->next) child->next->prev = child;
		parent->child = child;
		child->parent = parent;
		changeRagdollBoneJoint(child, physx::PxJointConcreteType::eREVOLUTE);
	}


	void findCloserChildren(Ragdoll& ragdoll, ComponentHandle cmp, Model* model, RagdollBone* bone)
	{
		for (auto* root = ragdoll.root; root; root = root->next)
		{
			if (root == bone) continue;

			auto* tmp = getPhyParent(cmp, model, root->pose_bone_idx);
			if (tmp != bone) continue;

			disconnect(ragdoll, root);
			connect(ragdoll, root, bone);
			break;
		}
		if (!bone->parent) return;

		for (auto* child = bone->parent->child; child; child = child->next)
		{
			if (child == bone) continue;

			auto* tmp = getPhyParent(cmp, model, bone->pose_bone_idx);
			if (tmp != bone) continue;

			disconnect(ragdoll, child);
			connect(ragdoll, child, bone);
		}
	}

	Transform getNewBoneTransform(const Model* model, int bone_idx, float& length)
	{
		auto& bone = model->getBone(bone_idx);
		auto& parent_bone = bone.parent_idx >= 0 ? model->getBone(bone.parent_idx) : bone;
		Matrix mtx = Matrix::IDENTITY;
		Vec3 dir = parent_bone.transform.pos - bone.transform.pos;
		length = dir.length();
		if (length > 0.001f)
		{
			mtx.setXVector(dir.normalized());
			Vec3 y = Vec3(-dir.y, dir.x, 0);
			if (y.squaredLength() < 0.001f)	y.set(dir.z, 0, -dir.x);
			mtx.setYVector(y.normalized());
			mtx.setZVector(crossProduct(dir, y).normalized());
		}
		mtx.setTranslation((bone.transform.pos + parent_bone.transform.pos) * 0.5f);
		return mtx.toTransform();
	}


	RagdollBoneHandle createRagdollBone(ComponentHandle cmp, uint32 bone_name_hash) override
	{
		auto* render_scene = static_cast<RenderScene*>(m_universe.getScene(RENDERER_HASH));
		ASSERT(render_scene);
		
		Entity entity = { cmp.index };
		ComponentHandle renderable = render_scene->getRenderableComponent(entity);
		ASSERT(isValid(renderable));
		Model* model = render_scene->getRenderableModel(renderable);
		ASSERT(model && model->isReady());
		auto iter = model->getBoneIndex(bone_name_hash);
		ASSERT(iter.isValid());

		auto* new_bone = LUMIX_NEW(m_allocator, RagdollBone);
		new_bone->child = new_bone->next = new_bone->prev = new_bone->parent = nullptr;
		new_bone->parent_joint = nullptr;
		new_bone->pose_bone_idx = iter.value();
		
		float bone_height;
		Transform transform = getNewBoneTransform(model, iter.value(), bone_height);
		
		new_bone->bind_transform = transform.inverted() * model->getBone(iter.value()).transform;
		transform = m_universe.getTransform(entity) * transform;

		physx::PxCapsuleGeometry geom;
		geom.halfHeight = bone_height * 0.3f;
		if (geom.halfHeight < 0.001f) geom.halfHeight = 1.0f;
		geom.radius = geom.halfHeight * 0.5f;

		physx::PxTransform px_transform = toPhysx(transform);
		new_bone->actor = physx::PxCreateDynamic(m_scene->getPhysics(), px_transform, geom, *m_default_material, 1.0f);
		new_bone->actor->setActorFlag(physx::PxActorFlag::eVISUALIZATION, true);
		m_scene->addActor(*new_bone->actor);
		updateFilterData(new_bone->actor, 0);

		auto& ragdoll = m_ragdolls[entity];
		new_bone->next = ragdoll.root;
		if (new_bone->next) new_bone->next->prev = new_bone;
		ragdoll.root = new_bone;
		auto* parent = getPhyParent(cmp, model, iter.value());
		if (parent) connect(ragdoll, new_bone, parent);

		findCloserChildren(ragdoll, cmp, model, new_bone);

		return new_bone;
	}


	void updateBone(const Transform& inv_root, RagdollBone* bone, Pose* pose)
	{
		if (!bone) return;
			
		physx::PxTransform bone_pose = bone->actor->getGlobalPose();

		auto tr = inv_root * Transform(fromPhysx(bone_pose.p), fromPhysx(bone_pose.q)) * bone->bind_transform;
		pose->rotations[bone->pose_bone_idx] = tr.rot;
		pose->positions[bone->pose_bone_idx] = tr.pos;
		updateBone(inv_root, bone->next, pose);
		updateBone(inv_root, bone->child, pose);
	}


	void updateRagdolls()
	{
		auto* render_scene = static_cast<RenderScene*>(m_universe.getScene(RENDERER_HASH));
		if (!render_scene) return;
		for (int i = 0, c = m_ragdolls.size(); i < c; ++i)
		{
			Ragdoll& ragdoll = m_ragdolls.at(i);
			Transform root_transform;
			root_transform.rot = m_universe.getRotation(ragdoll.entity);
			root_transform.pos = m_universe.getPosition(ragdoll.entity);
			ComponentHandle renderable = render_scene->getRenderableComponent(ragdoll.entity);
			if (!isValid(renderable)) continue;
			Pose* pose = render_scene->getPose(renderable);
			if (pose) updateBone(root_transform.inverted(), ragdoll.root, pose);
		}
	}


	void update(float time_delta, bool paused) override
	{
		if (!m_is_game_running || paused) return;
		
		applyQueuedForces();

		time_delta = Math::minimum(1 / 20.0f, time_delta);
		simulateScene(time_delta);
		fetchResults();
		updateRagdolls();
		updateDynamicActors();
		updateControllers(time_delta);
		
		render();
	}


	ComponentHandle getActorComponent(Entity entity) override
	{
		int idx = m_actors.find(entity);
		if (idx < 0) return INVALID_COMPONENT;
		return {entity.index};
	}


	void initJoints()
	{
		for (int i = 0, c = m_joints.size(); i < c; ++i)
		{
			auto& joint = m_joints.at(i);

			Entity entity = m_joints.getKey(i);

			physx::PxRigidActor* actors[2] = { nullptr, nullptr };
			int idx = m_actors.find(entity);
			if (idx >= 0) actors[0] = m_actors.at(idx)->physx_actor;
			idx = m_actors.find(joint.connected_body);
			if (idx >= 0) actors[1] = m_actors.at(idx)->physx_actor;
			if (!actors[0] || !actors[1]) continue;

			Vec3 pos0 = m_universe.getPosition(entity);
			Quat rot0 = m_universe.getRotation(entity);
			Vec3 pos1 = m_universe.getPosition(joint.connected_body);
			Quat rot1 = m_universe.getRotation(joint.connected_body);
			physx::PxTransform entity0_frame(toPhysx(pos0), toPhysx(rot0));
			physx::PxTransform entity1_frame(toPhysx(pos1), toPhysx(rot1));

			physx::PxTransform axis_local_frame1 = entity1_frame.getInverse() * entity0_frame * joint.local_frame0;

			joint.physx->setLocalPose(physx::PxJointActorIndex::eACTOR0, joint.local_frame0);
			joint.physx->setLocalPose(physx::PxJointActorIndex::eACTOR1, axis_local_frame1);
			joint.physx->setActors(actors[0], actors[1]);
			joint.physx->setConstraintFlag(physx::PxConstraintFlag::eVISUALIZATION, true);
		}
	}


	void startGame() override
	{
		auto* scene = m_universe.getScene(crc32("lua_script"));
		m_script_scene = static_cast<LuaScriptScene*>(scene);
		m_is_game_running = true;

		initJoints();
	}


	void stopGame() override
	{
		m_is_game_running = false;
	}


	float getControllerRadius(ComponentHandle cmp) override { return m_controllers[cmp.index].m_radius; }
	float getControllerHeight(ComponentHandle cmp) override { return m_controllers[cmp.index].m_height; }


	ComponentHandle getController(Entity entity) override
	{
		for (int i = 0; i < m_controllers.size(); ++i)
		{
			if (m_controllers[i].m_entity == entity)
			{
				return {i};
			}
		}
		return INVALID_COMPONENT;
	}


	void moveController(ComponentHandle cmp, const Vec3& v) override { m_controllers[cmp.index].m_frame_change += v; }


	static int LUA_raycast(lua_State* L)
	{
		auto* scene = LuaWrapper::checkArg<PhysicsSceneImpl*>(L, 1);
		Vec3 origin = LuaWrapper::checkArg<Vec3>(L, 2);
		Vec3 dir = LuaWrapper::checkArg<Vec3>(L, 3);

		RaycastHit hit;
		if (scene->raycastEx(origin, dir, FLT_MAX, hit))
		{
			LuaWrapper::pushLua(L, hit.entity != INVALID_ENTITY);
			LuaWrapper::pushLua(L, hit.entity);
			LuaWrapper::pushLua(L, hit.position);
			return 3;
		}
		LuaWrapper::pushLua(L, false);
		return 1;
	}


	Entity raycast(const Vec3& origin, const Vec3& dir) override
	{
		RaycastHit hit;
		if (raycastEx(origin, dir, FLT_MAX, hit)) return hit.entity;
		return INVALID_ENTITY;
	}


	bool raycastEx(const Vec3& origin, const Vec3& dir, float distance, RaycastHit& result) override
	{
		physx::PxVec3 physx_origin(origin.x, origin.y, origin.z);
		physx::PxVec3 unit_dir(dir.x, dir.y, dir.z);
		physx::PxReal max_distance = distance;
		physx::PxRaycastHit hit;

		const physx::PxSceneQueryFlags outputFlags =
			physx::PxSceneQueryFlag::eDISTANCE | physx::PxSceneQueryFlag::eIMPACT | physx::PxSceneQueryFlag::eNORMAL;

		bool status = m_scene->raycastSingle(physx_origin, unit_dir, max_distance, outputFlags, hit);
		result.normal.x = hit.normal.x;
		result.normal.y = hit.normal.y;
		result.normal.z = hit.normal.z;
		result.position.x = hit.position.x;
		result.position.y = hit.position.y;
		result.position.z = hit.position.z;
		result.entity = INVALID_ENTITY;
		if (hit.shape)
		{
			physx::PxRigidActor* actor = hit.shape->getActor();
			if (actor && actor->userData) result.entity = {(int)(intptr_t)actor->userData};
		}
		return status;
	}


	void onEntityMoved(Entity entity)
	{
		for (int i = 0, c = m_controllers.size(); i < c; ++i)
		{
			if (m_controllers[i].m_entity == entity)
			{
				Vec3 pos = m_universe.getPosition(entity);
				pos.y += m_controllers[i].m_height * 0.5f;
				pos.y += m_controllers[i].m_radius;
				physx::PxExtendedVec3 pvec(pos.x, pos.y, pos.z);
				m_controllers[i].m_controller->setPosition(pvec);
				return;
			}
		}

		int idx = m_actors.find(entity);
		if(idx >= 0)
		{
			Vec3 pos = m_universe.getPosition(entity);
			physx::PxVec3 pvec(pos.x, pos.y, pos.z);
			Quat q = m_universe.getRotation(entity);
			physx::PxQuat pquat(q.x, q.y, q.z, q.w);
			physx::PxTransform trans(pvec, pquat);
			m_actors.at(idx)->physx_actor->setGlobalPose(trans, false);
			return;
		}
	}


	void heightmapLoaded(Heightfield* terrain)
	{
		PROFILE_FUNCTION();
		Array<physx::PxHeightFieldSample> heights(m_allocator);

		int width = terrain->m_heightmap->width;
		int height = terrain->m_heightmap->height;
		heights.resize(width * height);
		int bytes_per_pixel = terrain->m_heightmap->bytes_per_pixel;
		if (bytes_per_pixel == 2)
		{
			PROFILE_BLOCK("copyData");
			const int16* LUMIX_RESTRICT data = (const int16*)terrain->m_heightmap->getData();
			for (int j = 0; j < height; ++j)
			{
				int idx = j * width;
				for (int i = 0; i < width; ++i)
				{
					int idx2 = j + i * height;
					heights[idx].height = physx::PxI16((int32)data[idx2] - 0x7fff);
					heights[idx].materialIndex0 = heights[idx].materialIndex1 = 0;
					heights[idx].setTessFlag();
					++idx;
				}
			}
		}
		else
		{
			PROFILE_BLOCK("copyData");
			const uint8* data = terrain->m_heightmap->getData();
			for (int j = 0; j < height; ++j)
			{
				for (int i = 0; i < width; ++i)
				{
					int idx = i + j * width;
					int idx2 = j + i * height;
					heights[idx].height = physx::PxI16((int32)data[idx2 * bytes_per_pixel] - 0x7f);
					heights[idx].materialIndex0 = heights[idx].materialIndex1 = 0;
					heights[idx].setTessFlag();
				}
			}
		}

		{ // PROFILE_BLOCK scope
			PROFILE_BLOCK("PhysX");
			physx::PxHeightFieldDesc hfDesc;
			hfDesc.format = physx::PxHeightFieldFormat::eS16_TM;
			hfDesc.nbColumns = width;
			hfDesc.nbRows = height;
			hfDesc.samples.data = &heights[0];
			hfDesc.samples.stride = sizeof(physx::PxHeightFieldSample);
			hfDesc.thickness = -1;

			physx::PxHeightField* heightfield = m_system->getPhysics()->createHeightField(hfDesc);
			float height_scale = bytes_per_pixel == 2 ? 1 / (256 * 256.0f - 1) : 1 / 255.0f;
			physx::PxHeightFieldGeometry hfGeom(heightfield,
				physx::PxMeshGeometryFlags(),
				height_scale * terrain->m_y_scale,
				terrain->m_xz_scale,
				terrain->m_xz_scale);
			if (terrain->m_actor)
			{
				physx::PxRigidActor* actor = terrain->m_actor;
				m_scene->removeActor(*actor);
				actor->release();
				terrain->m_actor = nullptr;
			}

			physx::PxTransform transform = toPhysx(m_universe.getTransform(terrain->m_entity));
			transform.p.y += terrain->m_y_scale * 0.5f;

			physx::PxRigidActor* actor;
			actor = PxCreateStatic(*m_system->getPhysics(), transform, hfGeom, *m_default_material);
			if (actor)
			{
				actor->userData = (void*)(intptr_t)terrain->m_entity.index;
				m_scene->addActor(*actor);
				terrain->m_actor = actor;

				physx::PxFilterData data;
				int terrain_layer = terrain->m_layer;
				data.word0 = 1 << terrain_layer;
				data.word1 = m_collision_filter[terrain_layer];
				physx::PxShape* shapes[8];
				int shapes_count = actor->getShapes(shapes, lengthOf(shapes));
				for (int i = 0; i < shapes_count; ++i)
				{
					shapes[i]->setSimulationFilterData(data);
				}
			}
			else
			{
				g_log_error.log("Physics") << "Could not create PhysX heightfield " << terrain->m_heightmap->getPath();
			}
		}
	}


	void addCollisionLayer() override 
	{
		m_layers_count = Math::minimum(lengthOf(m_layers_names), m_layers_count + 1);
	}


	void removeCollisionLayer() override
	{
		m_layers_count = Math::maximum(0, m_layers_count - 1);
		for (int i = 0; i < m_actors.size(); ++i)
		{
			auto* actor = m_actors.at(i);
			actor->layer = Math::minimum(m_layers_count - 1, actor->layer);
		}
		for (auto& controller : m_controllers)
		{
			if (controller.m_is_free) continue;
			controller.m_layer = Math::minimum(m_layers_count - 1, controller.m_layer);
		}
		for (auto* terrain : m_terrains)
		{
			if (!terrain) continue;
			if (!terrain->m_actor) continue;
			terrain->m_layer = Math::minimum(m_layers_count - 1, terrain->m_layer);
		}

		updateFilterData();
	}


	void setCollisionLayerName(int index, const char* name) override
	{
		copyString(m_layers_names[index], name);
	}


	const char* getCollisionLayerName(int index) override
	{
		return m_layers_names[index];
	}


	bool canLayersCollide(int layer1, int layer2) override
	{
		return (m_collision_filter[layer1] & (1 << layer2)) != 0;
	}


	void setLayersCanCollide(int layer1, int layer2, bool can_collide) override
	{
		if (can_collide)
		{
			m_collision_filter[layer1] |= 1 << layer2;
			m_collision_filter[layer2] |= 1 << layer1;
		}
		else
		{
			m_collision_filter[layer1] &= ~(1 << layer2);
			m_collision_filter[layer2] &= ~(1 << layer1);
		}

		updateFilterData();
	}


	void updateFilterData(physx::PxRigidActor* actor, int layer)
	{
		physx::PxFilterData data;
		data.word0 = 1 << layer;
		data.word1 = m_collision_filter[layer];
		physx::PxShape* shapes[8];
		int shapes_count = actor->getShapes(shapes, lengthOf(shapes));
		for (int i = 0; i < shapes_count; ++i)
		{
			shapes[i]->setSimulationFilterData(data);
		}
	}


	void updateFilterData()
	{
		for (int i = 0, c = m_actors.size(); i < c; ++i)
		{
			auto* actor = m_actors.at(i);
			physx::PxFilterData data;
			int actor_layer = actor->layer;
			data.word0 = 1 << actor_layer;
			data.word1 = m_collision_filter[actor_layer];
			physx::PxShape* shapes[8];
			int shapes_count = actor->physx_actor->getShapes(shapes, lengthOf(shapes));
			for (int i = 0; i < shapes_count; ++i)
			{
				shapes[i]->setSimulationFilterData(data);
			}
		}

		for (auto& controller : m_controllers)
		{
			if (controller.m_is_free) continue;

			physx::PxFilterData data;
			int controller_layer = controller.m_layer;
			data.word0 = 1 << controller_layer;
			data.word1 = m_collision_filter[controller_layer];
			physx::PxShape* shapes[8];
			int shapes_count = controller.m_controller->getActor()->getShapes(shapes, lengthOf(shapes));
			for (int i = 0; i < shapes_count; ++i)
			{
				shapes[i]->setSimulationFilterData(data);
			}
		}

		for (auto* terrain : m_terrains)
		{
			if (!terrain) continue;
			if (!terrain->m_actor) continue;

			physx::PxFilterData data;
			int terrain_layer = terrain->m_layer;
			data.word0 = 1 << terrain_layer;
			data.word1 = m_collision_filter[terrain_layer];
			physx::PxShape* shapes[8];
			int shapes_count = terrain->m_actor->getShapes(shapes, lengthOf(shapes));
			for (int i = 0; i < shapes_count; ++i)
			{
				shapes[i]->setSimulationFilterData(data);
			}
		}
	}


	int getCollisionsLayersCount() const override
	{
		return m_layers_count;
	}


	bool isDynamic(ComponentHandle cmp) override
	{
		RigidActor* actor = m_actors[{cmp.index}];
		return isDynamic(actor);
	}


	bool isDynamic(RigidActor* actor)
	{
		for (int i = 0, c = m_dynamic_actors.size(); i < c; ++i)
		{
			if (m_dynamic_actors[i] == actor)
			{
				return true;
			}
		}
		return false;
	}


	Vec3 getHalfExtents(ComponentHandle cmp) override
	{
		Vec3 size;
		physx::PxRigidActor* actor = m_actors[{cmp.index}]->physx_actor;
		physx::PxShape* shapes;
		if (actor->getNbShapes() == 1 && actor->getShapes(&shapes, 1))
		{
			physx::PxVec3& half = shapes->getGeometry().box().halfExtents;
			size.x = half.x;
			size.y = half.y;
			size.z = half.z;
		}
		return size;
	}


	void setHalfExtents(ComponentHandle cmp, const Vec3& size) override
	{
		physx::PxRigidActor* actor = m_actors[{cmp.index}]->physx_actor;
		physx::PxShape* shapes;
		if (actor->getNbShapes() == 1 && actor->getShapes(&shapes, 1))
		{
			physx::PxBoxGeometry box;
			bool is_box = shapes->getBoxGeometry(box);
			ASSERT(is_box);
			physx::PxVec3& half = box.halfExtents;
			half.x = Math::maximum(0.01f, size.x);
			half.y = Math::maximum(0.01f, size.y);
			half.z = Math::maximum(0.01f, size.z);
			shapes->setGeometry(box);
		}
	}


	void setIsDynamic(ComponentHandle cmp, bool new_value) override
	{
		RigidActor* actor = m_actors[{cmp.index}];
		int dynamic_index = m_dynamic_actors.indexOf(actor);
		bool is_dynamic = dynamic_index != -1;
		if (is_dynamic == new_value) return;

		actor->is_dynamic = new_value;
		if (new_value)
		{
			m_dynamic_actors.push(actor);
		}
		else
		{
			m_dynamic_actors.eraseItemFast(actor);
		}
		physx::PxShape* shapes;
		if (actor->physx_actor->getNbShapes() == 1 && actor->physx_actor->getShapes(&shapes, 1, 0))
		{
			physx::PxGeometryHolder geom = shapes->getGeometry();
			physx::PxTransform transform = toPhysx(m_universe.getTransform(actor->entity));

			physx::PxRigidActor* physx_actor;
			if (new_value)
			{
				physx_actor = PxCreateDynamic(*m_system->getPhysics(), transform, geom.any(), *m_default_material, 1.0f);
			}
			else
			{
				physx_actor = PxCreateStatic(*m_system->getPhysics(), transform, geom.any(), *m_default_material);
			}
			ASSERT(actor);
			physx_actor->userData = (void*)(intptr_t)actor->entity.index;
			actor->setPhysxActor(physx_actor);
		}
	}


	void serializeActor(OutputBlob& serializer, RigidActor* actor)
	{
		serializer.write(actor->layer);
		physx::PxShape* shapes;
		auto* px_actor = actor->physx_actor;
		auto* resource = actor->resource;
		serializer.write((int32)actor->type);
		switch (actor->type)
		{
			case ActorType::BOX:
			{
				ASSERT(px_actor->getNbShapes() == 1);
				px_actor->getShapes(&shapes, 1);
				physx::PxBoxGeometry geom;
				if (!shapes->getBoxGeometry(geom)) ASSERT(false);
				serializer.write(geom.halfExtents.x);
				serializer.write(geom.halfExtents.y);
				serializer.write(geom.halfExtents.z);
				break;
			}
			case ActorType::SPHERE:
			{
				ASSERT(px_actor->getNbShapes() == 1);
				px_actor->getShapes(&shapes, 1);
				physx::PxSphereGeometry geom;
				if (!shapes->getSphereGeometry(geom)) ASSERT(false);
				serializer.write(geom.radius);
				break;
			}
			case ActorType::CAPSULE:
			{
				ASSERT(px_actor->getNbShapes() == 1);
				px_actor->getShapes(&shapes, 1);
				physx::PxCapsuleGeometry geom;
				if (!shapes->getCapsuleGeometry(geom)) ASSERT(false);
				serializer.write(geom.halfHeight);
				serializer.write(geom.radius);
				break;
			}
			case ActorType::MESH: serializer.writeString(resource ? resource->getPath().c_str() : ""); break;
			default: ASSERT(false);
		}
	}


	void deserializeActor(InputBlob& serializer, RigidActor* actor, int version)
	{
		ComponentHandle cmp = {actor->entity.index};
		actor->layer = 0;
		if (version > (int)PhysicsSceneVersion::LAYERS) serializer.read(actor->layer);

		serializer.read((int32&)actor->type);

		switch (actor->type)
		{
			case ActorType::BOX:
			{
				physx::PxBoxGeometry box_geom;
				physx::PxTransform transform = toPhysx(m_universe.getTransform(actor->entity));
				serializer.read(box_geom.halfExtents);
				physx::PxRigidActor* physx_actor;
				if (isDynamic(cmp))
				{
					physx_actor =
						PxCreateDynamic(*m_system->getPhysics(), transform, box_geom, *m_default_material, 1.0f);
				}
				else
				{
					physx_actor = PxCreateStatic(*m_system->getPhysics(), transform, box_geom, *m_default_material);
				}
				actor->setPhysxActor(physx_actor);
				m_universe.addComponent(actor->entity, BOX_ACTOR_TYPE, this, cmp);
			}
			break;
			case ActorType::SPHERE:
			{
				physx::PxSphereGeometry sphere_geom;
				physx::PxTransform transform = toPhysx(m_universe.getTransform(actor->entity));
				serializer.read(sphere_geom.radius);
				physx::PxRigidActor* physx_actor;
				if (isDynamic(cmp))
				{
					physx_actor =
						PxCreateDynamic(*m_system->getPhysics(), transform, sphere_geom, *m_default_material, 1.0f);
				}
				else
				{
					physx_actor = PxCreateStatic(*m_system->getPhysics(), transform, sphere_geom, *m_default_material);
				}
				actor->setPhysxActor(physx_actor);
				m_universe.addComponent(actor->entity, SPHERE_ACTOR_TYPE, this, cmp);
			}
			break;
			case ActorType::CAPSULE:
			{
				physx::PxCapsuleGeometry capsule_geom;
				physx::PxTransform transform = toPhysx(m_universe.getTransform(actor->entity));
				serializer.read(capsule_geom.halfHeight);
				serializer.read(capsule_geom.radius);
				physx::PxRigidActor* physx_actor;
				if (isDynamic(cmp))
				{
					physx_actor =
						PxCreateDynamic(*m_system->getPhysics(), transform, capsule_geom, *m_default_material, 1.0f);
				}
				else
				{
					physx_actor = PxCreateStatic(*m_system->getPhysics(), transform, capsule_geom, *m_default_material);
				}
				actor->setPhysxActor(physx_actor);
				m_universe.addComponent(actor->entity, CAPSULE_ACTOR_TYPE, this, cmp);
			}
			break;
			case ActorType::MESH:
			{
				char tmp[MAX_PATH_LENGTH];
				serializer.readString(tmp, sizeof(tmp));
				ResourceManagerBase* manager = m_engine->getResourceManager().get(PHYSICS_HASH);
				auto* geometry = manager->load(Lumix::Path(tmp));
				actor->setResource(static_cast<PhysicsGeometry*>(geometry));
				m_universe.addComponent(actor->entity, MESH_ACTOR_TYPE, this, cmp);
			}
			break;
			default:
				ASSERT(false);
				break;
		}
	}


	void serialize(OutputBlob& serializer) override
	{
		serializer.write(m_layers_count);
		serializer.write(m_layers_names);
		serializer.write(m_collision_filter);
		serializer.write((int32)m_actors.size());
		for (int i = 0, c = m_actors.size(); i < c; ++i)
		{
			RigidActor* actor = m_actors.at(i);
			serializer.write(isDynamic({actor->entity.index}));
			serializer.write(actor->entity);
			serializeActor(serializer, actor);
		}
		serializer.write((int32)m_controllers.size());
		for (int i = 0; i < m_controllers.size(); ++i)
		{
			serializer.write(m_controllers[i].m_entity);
			serializer.write(m_controllers[i].m_is_free);
			if (!m_controllers[i].m_is_free)
			{
				serializer.write(m_controllers[i].m_layer);
			}
		}
		serializer.write((int32)m_terrains.size());
		for (int i = 0; i < m_terrains.size(); ++i)
		{
			if (m_terrains[i])
			{
				serializer.write(true);
				serializer.write(m_terrains[i]->m_entity);
				serializer.writeString(m_terrains[i]->m_heightmap ? m_terrains[i]->m_heightmap->getPath().c_str() : "");
				serializer.write(m_terrains[i]->m_xz_scale);
				serializer.write(m_terrains[i]->m_y_scale);
				serializer.write(m_terrains[i]->m_layer);
			}
			else
			{
				serializer.write(false);
			}
		}
		serializeRagdolls(serializer);
		serializeJoints(serializer);
	}


	void serializeRagdollJoint(RagdollBone* bone, OutputBlob& serializer)
	{
		serializer.write(bone->parent_joint != nullptr);
		if (!bone->parent_joint) return;

		serializer.write((int)bone->parent_joint->getConcreteType());
		serializer.write(bone->parent_joint->getLocalPose(physx::PxJointActorIndex::eACTOR0));
		serializer.write(bone->parent_joint->getLocalPose(physx::PxJointActorIndex::eACTOR1));

		switch ((physx::PxJointConcreteType::Enum)bone->parent_joint->getConcreteType())
		{
			case physx::PxJointConcreteType::eFIXED: break;
			case physx::PxJointConcreteType::eDISTANCE:
			{
				auto* joint = bone->parent_joint->is<physx::PxDistanceJoint>();
				serializer.write(joint->getMinDistance());
				serializer.write(joint->getMaxDistance());
				serializer.write(joint->getTolerance());
				serializer.write(joint->getStiffness());
				serializer.write(joint->getDamping());
				uint32 flags = (physx::PxU32)joint->getDistanceJointFlags();
				serializer.write(flags);
				break;
			}
			case physx::PxJointConcreteType::eREVOLUTE:
			{
				auto* joint = bone->parent_joint->is<physx::PxRevoluteJoint>();
				serializer.write(joint->getLimit());
				uint32 flags = (physx::PxU32)joint->getRevoluteJointFlags();
				serializer.write(flags);
				break;
			}
			default: ASSERT(false); break;
		}
	}


	void serializeRagdollBone(RagdollBone* bone, OutputBlob& serializer)
	{
		if (!bone)
		{
			serializer.write(-1);
			return;
		}
		serializer.write(bone->pose_bone_idx);
		physx::PxTransform pose = bone->actor->getGlobalPose();
		serializer.write(fromPhysx(pose));
		serializer.write(bone->bind_transform);

		physx::PxShape* shape;
		int shape_count = bone->actor->getShapes(&shape, 1);
		ASSERT(shape_count == 1);
		physx::PxBoxGeometry box_geom;
		if (shape->getBoxGeometry(box_geom))
		{
			serializer.write(RagdollBone::BOX);
			serializer.write(box_geom.halfExtents);
		}
		else
		{
			physx::PxCapsuleGeometry capsule_geom;
			bool is_capsule = shape->getCapsuleGeometry(capsule_geom);
			ASSERT(is_capsule);
			serializer.write(RagdollBone::CAPSULE);
			serializer.write(capsule_geom.halfHeight);
			serializer.write(capsule_geom.radius);
		}

		serializeRagdollBone(bone->child, serializer);
		serializeRagdollBone(bone->next, serializer);

		serializeRagdollJoint(bone, serializer);
	}


	void deserializeRagdollJoint(RagdollBone* bone, InputBlob& serializer)
	{
		bool has_joint;
		serializer.read(has_joint);
		if (!has_joint) return;

		int type;
		serializer.read(type);
		changeRagdollBoneJoint(bone, type);

		physx::PxTransform local_poses[2];
		serializer.read(local_poses);
		bone->parent_joint->setLocalPose(physx::PxJointActorIndex::eACTOR0, local_poses[0]);
		bone->parent_joint->setLocalPose(physx::PxJointActorIndex::eACTOR1, local_poses[1]);

		switch ((physx::PxJointConcreteType::Enum)type)
		{
			case physx::PxJointConcreteType::eFIXED: break;
			case physx::PxJointConcreteType::eDISTANCE:
			{
				auto* joint = bone->parent_joint->is<physx::PxDistanceJoint>();
				physx::PxReal value;
				serializer.read(value);
				joint->setMinDistance(value);
				serializer.read(value);
				joint->setMaxDistance(value);
				serializer.read(value);
				joint->setTolerance(value);
				serializer.read(value);
				joint->setStiffness(value);
				serializer.read(value);
				joint->setDamping(value);
				uint32 flags;
				serializer.read(flags);
				joint->setDistanceJointFlags((physx::PxDistanceJointFlags)flags);
				break;
			}
			case physx::PxJointConcreteType::eREVOLUTE:
			{
				auto* joint = bone->parent_joint->is<physx::PxRevoluteJoint>();
				physx::PxJointAngularLimitPair limit(0, 0);
				serializer.read(limit);
				joint->setLimit(limit);
				uint32 flags;
				serializer.read(flags);
				joint->setRevoluteJointFlags((physx::PxRevoluteJointFlags)flags);
				break;
			}
			default: ASSERT(false); break;
		}
	}


	RagdollBone* deserializeRagdollBone(RagdollBone* parent, InputBlob& serializer)
	{
		int pose_bone_idx;
		serializer.read(pose_bone_idx);
		if (pose_bone_idx < 0) return nullptr;
		auto* bone = LUMIX_NEW(m_allocator, RagdollBone);
		bone->pose_bone_idx = pose_bone_idx;
		bone->parent_joint = nullptr;
		bone->prev = nullptr;
		Transform transform;
		serializer.read(transform);
		serializer.read(bone->bind_transform);

		physx::PxTransform px_transform = toPhysx(transform);

		RagdollBone::Type type;
		serializer.read(type);
		
		switch (type)
		{
			case RagdollBone::CAPSULE:
			{
				physx::PxCapsuleGeometry shape;
				serializer.read(shape.halfHeight);
				serializer.read(shape.radius);
				bone->actor =
					physx::PxCreateDynamic(m_scene->getPhysics(), px_transform, shape, *m_default_material, 1.0f);
				break;
			}
			case RagdollBone::BOX:
			{
				physx::PxBoxGeometry shape;
				serializer.read(shape.halfExtents);
				bone->actor =
					physx::PxCreateDynamic(m_scene->getPhysics(), px_transform, shape, *m_default_material, 1.0f);
				break;
			}
			default: ASSERT(false); break;
		}
		bone->actor->setActorFlag(physx::PxActorFlag::eVISUALIZATION, true);
		m_scene->addActor(*bone->actor);
		updateFilterData(bone->actor, 0);

		bone->parent = parent;

		bone->child = deserializeRagdollBone(bone, serializer);
		bone->next = deserializeRagdollBone(parent, serializer);
		if(bone->next) bone->next->prev = bone;

		deserializeRagdollJoint(bone, serializer);

		return bone;
	}



	void serializeRagdolls(OutputBlob& serializer)
	{
		serializer.write(m_ragdolls.size());
		for (int i = 0, c = m_ragdolls.size(); i < c; ++i)
		{
			serializer.write(m_ragdolls.getKey(i));
			const Ragdoll& ragdoll = m_ragdolls.at(i);
			serializeRagdollBone(ragdoll.root, serializer);
		}
	}


	void serializeJoints(OutputBlob& serializer)
	{
		serializer.write(m_joints.size());
		for (int i = 0; i < m_joints.size(); ++i)
		{
			const Joint& joint = m_joints.at(i);
			serializer.write(m_joints.getKey(i));
			serializer.write((int)joint.physx->getConcreteType());
			serializer.write(joint.connected_body);
			serializer.write(joint.local_frame0);
			switch ((physx::PxJointConcreteType::Enum)joint.physx->getConcreteType())
			{
				case physx::PxJointConcreteType::eSPHERICAL:
				{
					auto* px_joint = static_cast<physx::PxSphericalJoint*>(joint.physx);
					uint32 flags = (uint32)px_joint->getSphericalJointFlags();
					serializer.write(flags);
					auto limit = px_joint->getLimitCone();
					serializer.write(limit);
					break;
				}
				case physx::PxJointConcreteType::eREVOLUTE:
				{
					auto* px_joint = static_cast<physx::PxRevoluteJoint*>(joint.physx);
					uint32 flags = (uint32)px_joint->getRevoluteJointFlags();
					serializer.write(flags);
					auto limit = px_joint->getLimit();
					serializer.write(limit);
					break;
				}
				case physx::PxJointConcreteType::eDISTANCE:
				{
					auto* px_joint = static_cast<physx::PxDistanceJoint*>(joint.physx);
					uint32 flags = (uint32)px_joint->getDistanceJointFlags();
					serializer.write(flags);
					serializer.write(px_joint->getDamping());
					serializer.write(px_joint->getStiffness());
					serializer.write(px_joint->getTolerance());
					serializer.write(px_joint->getMinDistance());
					serializer.write(px_joint->getMaxDistance());
					break;
				}
				case physx::PxJointConcreteType::eD6:
				{
					auto* px_joint = static_cast<physx::PxD6Joint*>(joint.physx);
					serializer.write(px_joint->getMotion(physx::PxD6Axis::eX));
					serializer.write(px_joint->getMotion(physx::PxD6Axis::eY));
					serializer.write(px_joint->getMotion(physx::PxD6Axis::eZ));
					serializer.write(px_joint->getMotion(physx::PxD6Axis::eSWING1));
					serializer.write(px_joint->getMotion(physx::PxD6Axis::eSWING2));
					serializer.write(px_joint->getMotion(physx::PxD6Axis::eTWIST));
					serializer.write(px_joint->getLinearLimit());
					serializer.write(px_joint->getSwingLimit());
					serializer.write(px_joint->getTwistLimit());
					break;
				}
				default: ASSERT(false); break;
			}
		}
	}



	void deserializeActors(InputBlob& serializer, int version)
	{
		int32 count;
		m_dynamic_actors.clear();
		serializer.read(count);
		for (int i = 0; i < m_actors.size(); ++i)
		{
			m_actors.at(i)->setPhysxActor(nullptr);
			LUMIX_DELETE(m_allocator, m_actors.at(i));
		}
		int old_size = m_actors.size();
		m_actors.clear();
		m_actors.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			RigidActor* actor = LUMIX_NEW(m_allocator, RigidActor)(*this, ActorType::BOX);
			serializer.read(actor->is_dynamic);
			serializer.read(actor->entity);
			if (!isValid(actor->entity))
			{
				LUMIX_DELETE(m_allocator, actor);
				continue;
			}
			if (actor->is_dynamic) m_dynamic_actors.push(actor);
			m_actors.insert(actor->entity, actor);
			deserializeActor(serializer, actor, version);
		}
	}


	void deserializeControllers(InputBlob& serializer, int version)
	{
		int32 count;
		serializer.read(count);
		for (int i = 0; i < m_controllers.size(); ++i)
		{
			if (!m_controllers[i].m_is_free)
			{
				m_controllers[i].m_controller->release();
			}
		}
		m_controllers.clear();
		for (int i = 0; i < count; ++i)
		{
			Entity e;
			bool is_free;
			serializer.read(e);
			serializer.read(is_free);

			Controller& c = m_controllers.emplace();
			c.m_is_free = is_free;
			c.m_frame_change.set(0, 0, 0);

			if (is_free) continue;

			if (version > (int)PhysicsSceneVersion::LAYERS)
			{
				serializer.read(c.m_layer);
			}
			else
			{
				c.m_layer = 0;
			}
			physx::PxCapsuleControllerDesc cDesc;
			cDesc.material = m_default_material;
			cDesc.height = 1.8f;
			cDesc.radius = 0.25f;
			cDesc.slopeLimit = 0.0f;
			cDesc.contactOffset = 0.1f;
			cDesc.stepOffset = 0.02f;
			cDesc.callback = nullptr;
			cDesc.behaviorCallback = nullptr;
			Vec3 position = m_universe.getPosition(e);
			cDesc.position.set(position.x, position.y - cDesc.height * 0.5f, position.z);
			c.m_controller = m_controller_manager->createController(*m_system->getPhysics(), m_scene, cDesc);
			c.m_entity = e;
			m_universe.addComponent(e, CONTROLLER_TYPE, this, {i});
		}
	}


	void destroySkeleton(RagdollBone* bone)
	{
		if (!bone) return;
		destroySkeleton(bone->next);
		destroySkeleton(bone->child);
		if (bone->parent_joint) bone->parent_joint->release();
		if (bone->actor) bone->actor->release();
		LUMIX_DELETE(m_allocator, bone);
	}


	void clearRagdolls()
	{
		for (int i = 0, c = m_ragdolls.size(); i < c; ++i)
		{
			destroySkeleton(m_ragdolls.at(i).root);
		}
		m_ragdolls.clear();
	}


	void deserializeRagdolls(InputBlob& serializer, int version)
	{
		if (version <= int(PhysicsSceneVersion::RAGDOLLS)) return;
			
		clearRagdolls();
		int count;
		serializer.read(count);
		m_ragdolls.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			Entity entity;
			serializer.read(entity);
			int idx = m_ragdolls.insert(entity, Ragdoll());
			Ragdoll& ragdoll = m_ragdolls.at(i);
			ragdoll.entity = entity;
			ragdoll.root = deserializeRagdollBone(nullptr, serializer);
			ComponentHandle cmp = {ragdoll.entity.index};
			m_universe.addComponent(ragdoll.entity, RAGDOLL_TYPE, this, cmp);
		}
	}


	void deserializeJoints(InputBlob& serializer, int version)
	{
		if (version <= int(PhysicsSceneVersion::JOINT_REFACTOR)) return;

		int count;
		serializer.read(count);
		for (int i = 0; i < m_joints.size(); ++i) m_joints.at(i).physx->release();
		m_joints.clear();
		m_joints.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			Entity entity;
			serializer.read(entity);
			Joint joint;
			int type;
			serializer.read(type);
			serializer.read(joint.connected_body);
			serializer.read(joint.local_frame0);
			ComponentType cmp_type;
			switch (physx::PxJointConcreteType::Enum(type))
			{
				case physx::PxJointConcreteType::eSPHERICAL:
				{
					cmp_type = SPHERICAL_JOINT_TYPE;
					auto* px_joint = physx::PxSphericalJointCreate(m_scene->getPhysics(),
						m_dummy_actor,
						joint.local_frame0,
						nullptr,
						physx::PxTransform::createIdentity());
					joint.physx = px_joint;
					uint32 flags;
					serializer.read(flags);
					px_joint->setSphericalJointFlags(physx::PxSphericalJointFlags(flags));
					physx::PxJointLimitCone limit(0, 0);
					serializer.read(limit);
					px_joint->setLimitCone(limit);
					break;
				}
				case physx::PxJointConcreteType::eREVOLUTE:
				{
					cmp_type = HINGE_JOINT_TYPE;
					auto* px_joint = physx::PxRevoluteJointCreate(m_scene->getPhysics(),
						m_dummy_actor,
						joint.local_frame0,
						nullptr,
						physx::PxTransform::createIdentity());
					joint.physx = px_joint;
					uint32 flags;
					serializer.read(flags);
					px_joint->setRevoluteJointFlags(physx::PxRevoluteJointFlags(flags));
					physx::PxJointAngularLimitPair limit(0, 0);
					serializer.read(limit);
					px_joint->setLimit(limit);
					break;
				}
				case physx::PxJointConcreteType::eDISTANCE:
				{
					cmp_type = DISTANCE_JOINT_TYPE;
					auto* px_joint = physx::PxDistanceJointCreate(m_scene->getPhysics(),
						m_dummy_actor,
						joint.local_frame0,
						nullptr,
						physx::PxTransform::createIdentity());
					joint.physx = px_joint;
					uint32 flags;
					serializer.read(flags);
					px_joint->setDistanceJointFlags(physx::PxDistanceJointFlags(flags));
					float tmp;
					serializer.read(tmp);
					px_joint->setDamping(tmp);
					serializer.read(tmp);
					px_joint->setStiffness(tmp);
					serializer.read(tmp);
					px_joint->setTolerance(tmp);
					serializer.read(tmp);
					px_joint->setMinDistance(tmp);
					serializer.read(tmp);
					px_joint->setMaxDistance(tmp);
					break;
				}
				case physx::PxJointConcreteType::eD6:
				{
					cmp_type = D6_JOINT_TYPE;
					auto* px_joint = physx::PxD6JointCreate(m_scene->getPhysics(),
						m_dummy_actor,
						joint.local_frame0,
						nullptr,
						physx::PxTransform::createIdentity());
					joint.physx = px_joint;
					int motions[6];
					serializer.read(motions);
					px_joint->setMotion(physx::PxD6Axis::eX, (physx::PxD6Motion::Enum)motions[0]);
					px_joint->setMotion(physx::PxD6Axis::eY, (physx::PxD6Motion::Enum)motions[1]);
					px_joint->setMotion(physx::PxD6Axis::eZ, (physx::PxD6Motion::Enum) motions[2]);
					px_joint->setMotion(physx::PxD6Axis::eSWING1, (physx::PxD6Motion::Enum)motions[3]);
					px_joint->setMotion(physx::PxD6Axis::eSWING2, (physx::PxD6Motion::Enum)motions[4]);
					px_joint->setMotion(physx::PxD6Axis::eTWIST, (physx::PxD6Motion::Enum)motions[5]);
					physx::PxJointLinearLimit linear_limit(0, physx::PxSpring(0, 0));
					serializer.read(linear_limit);
					px_joint->setLinearLimit(linear_limit);
					physx::PxJointLimitCone swing_limit(0, 0);
					serializer.read(swing_limit);
					px_joint->setSwingLimit(swing_limit);
					physx::PxJointAngularLimitPair twist_limit(0, 0);
					serializer.read(twist_limit);
					px_joint->setTwistLimit(twist_limit);
					break;
				}
				default: ASSERT(false); break;
			}

			m_joints.insert(entity, joint);
			ComponentHandle cmp = {entity.index};
			m_universe.addComponent(entity, cmp_type, this, cmp);
		}

	}


	void deserializeTerrains(InputBlob& serializer, int version)
	{
		int32 count;
		serializer.read(count);
		for (int i = count; i < m_terrains.size(); ++i)
		{
			LUMIX_DELETE(m_allocator, m_terrains[i]);
			m_terrains[i] = nullptr;
		}
		int old_size = m_terrains.size();
		m_terrains.resize(count);
		for (int i = old_size; i < count; ++i)
		{
			m_terrains[i] = nullptr;
		}
		for (int i = 0; i < count; ++i)
		{
			bool exists;
			serializer.read(exists);
			if (exists)
			{
				if (!m_terrains[i])
				{
					m_terrains[i] = LUMIX_NEW(m_allocator, Heightfield);
				}
				m_terrains[i]->m_scene = this;
				serializer.read(m_terrains[i]->m_entity);
				char tmp[MAX_PATH_LENGTH];
				serializer.readString(tmp, MAX_PATH_LENGTH);
				serializer.read(m_terrains[i]->m_xz_scale);
				serializer.read(m_terrains[i]->m_y_scale);
				if (version > (int)PhysicsSceneVersion::LAYERS)
				{
					serializer.read(m_terrains[i]->m_layer);
				}
				else
				{
					m_terrains[i]->m_layer = 0;
				}

				if (m_terrains[i]->m_heightmap == nullptr ||
					!equalStrings(tmp, m_terrains[i]->m_heightmap->getPath().c_str()))
				{
					setHeightmap({i}, Path(tmp));
				}
				m_universe.addComponent(m_terrains[i]->m_entity, HEIGHTFIELD_TYPE, this, {i});
			}
		}
	}


	void deserialize(InputBlob& serializer, int version) override
	{
		if (version > (int)PhysicsSceneVersion::LAYERS)
		{
			serializer.read(m_layers_count);
			serializer.read(m_layers_names);
			serializer.read(m_collision_filter);
		}

		deserializeActors(serializer, version);
		deserializeControllers(serializer, version);
		deserializeTerrains(serializer, version);
		deserializeRagdolls(serializer, version);
		deserializeJoints(serializer, version);

		updateFilterData();
	}


	PhysicsSystem& getSystem() const override { return *m_system; }


	int getVersion() const override { return (int)PhysicsSceneVersion::LATEST; }


	float getActorSpeed(ComponentHandle cmp) override
	{
		auto* actor = m_actors[{cmp.index}];
		if (!actor->is_dynamic)
		{
			g_log_warning.log("Physics") << "Trying to get speed of static object";
			return 0;
		}

		auto* physx_actor = static_cast<physx::PxRigidDynamic*>(actor->physx_actor);
		if (!physx_actor) return 0;
		return physx_actor->getLinearVelocity().magnitude();
	}


	void putToSleep(ComponentHandle cmp) override
	{
		auto* actor = m_actors[{cmp.index}];
		if (!actor->is_dynamic)
		{
			g_log_warning.log("Physics") << "Trying to put static object to sleep";
			return;
		}

		auto* physx_actor = static_cast<physx::PxRigidDynamic*>(actor->physx_actor);
		if (!physx_actor) return;
		physx_actor->putToSleep();
	}


	void applyForceToActor(ComponentHandle cmp, const Vec3& force) override
	{
		auto& i = m_queued_forces.emplace();
		i.cmp = cmp;
		i.force = force;
	}


	static physx::PxFilterFlags filterShader(
		physx::PxFilterObjectAttributes attributes0, physx::PxFilterData filterData0,
		physx::PxFilterObjectAttributes attributes1, physx::PxFilterData filterData1,
		physx::PxPairFlags& pairFlags, const void* constantBlock, physx::PxU32 constantBlockSize)
	{
		if (physx::PxFilterObjectIsTrigger(attributes0) || physx::PxFilterObjectIsTrigger(attributes1))
		{
			pairFlags = physx::PxPairFlag::eTRIGGER_DEFAULT;
			return physx::PxFilterFlag::eDEFAULT;
		}

		if (!(filterData0.word0 & filterData1.word1) || !(filterData1.word0 & filterData0.word1))
		{
			return physx::PxFilterFlag::eKILL;
		}
		pairFlags = physx::PxPairFlag::eCONTACT_DEFAULT | physx::PxPairFlag::eNOTIFY_TOUCH_FOUND |
					physx::PxPairFlag::eNOTIFY_CONTACT_POINTS;
		return physx::PxFilterFlag::eDEFAULT;
	}


	struct QueuedForce
	{
		ComponentHandle cmp;
		Vec3 force;
	};


	struct Controller
	{
		physx::PxController* m_controller;
		Entity m_entity;
		Vec3 m_frame_change;
		float m_radius;
		float m_height;
		bool m_is_free;
		int m_layer;
	};

	IAllocator& m_allocator;

	Universe& m_universe;
	Engine* m_engine;
	ContactCallback m_contact_callback;
	physx::PxScene* m_scene;
	LuaScriptScene* m_script_scene;
	PhysicsSystem* m_system;
	physx::PxRigidDynamic* m_dummy_actor;
	physx::PxControllerManager* m_controller_manager;
	physx::PxMaterial* m_default_material;
	AssociativeArray<Entity, RigidActor*> m_actors;
	Array<RigidActor*> m_dynamic_actors;
	AssociativeArray<Entity, Ragdoll> m_ragdolls;
	AssociativeArray<Entity, Joint> m_joints;
	bool m_is_game_running;
	uint32 m_debug_visualization_flags;

	Array<QueuedForce> m_queued_forces;
	Array<Controller> m_controllers;
	Array<Heightfield*> m_terrains;
	uint32 m_collision_filter[32];
	char m_layers_names[32][30];
	int m_layers_count;
};


PhysicsScene* PhysicsScene::create(PhysicsSystem& system,
	Universe& context,
	Engine& engine,
	IAllocator& allocator)
{
	PhysicsSceneImpl* impl = LUMIX_NEW(allocator, PhysicsSceneImpl)(context, allocator);
	impl->m_universe.entityTransformed().bind<PhysicsSceneImpl, &PhysicsSceneImpl::onEntityMoved>(
		impl);
	impl->m_engine = &engine;
	physx::PxSceneDesc sceneDesc(system.getPhysics()->getTolerancesScale());
	sceneDesc.gravity = physx::PxVec3(0.0f, -9.8f, 0.0f);
	if (!sceneDesc.cpuDispatcher)
	{
		physx::PxDefaultCpuDispatcher* cpu_dispatcher = physx::PxDefaultCpuDispatcherCreate(1);
		if (!cpu_dispatcher)
		{
			g_log_error.log("Physics") << "PxDefaultCpuDispatcherCreate failed!";
		}
		sceneDesc.cpuDispatcher = cpu_dispatcher;
	}

	sceneDesc.filterShader = impl->filterShader;
	sceneDesc.simulationEventCallback = &impl->m_contact_callback;

	impl->m_scene = system.getPhysics()->createScene(sceneDesc);
	if (!impl->m_scene)
	{
		LUMIX_DELETE(allocator, impl);
		return nullptr;
	}

	impl->m_controller_manager = PxCreateControllerManager(*impl->m_scene);

	impl->m_system = &system;
	impl->m_default_material =
		impl->m_system->getPhysics()->createMaterial(0.5, 0.5, 0.5);
	physx::PxSphereGeometry geom(1);
	impl->m_dummy_actor = physx::PxCreateDynamic(
		impl->m_scene->getPhysics(), physx::PxTransform::createIdentity(), geom, *impl->m_default_material, 1);
	return impl;
}


void PhysicsScene::destroy(PhysicsScene* scene)
{
	PhysicsSceneImpl* impl = static_cast<PhysicsSceneImpl*>(scene);
	impl->m_controller_manager->release();
	impl->m_default_material->release();
	impl->m_dummy_actor->release();
	impl->m_scene->release();
	LUMIX_DELETE(impl->m_allocator, scene);
}


void PhysicsSceneImpl::RigidActor::onStateChanged(Resource::State, Resource::State new_state)
{
	if (new_state == Resource::State::READY)
	{
		setPhysxActor(nullptr);

		physx::PxTransform transform = toPhysx(scene.getUniverse().getTransform(entity));

		physx::PxRigidActor* actor;
		bool is_dynamic = scene.isDynamic(this);
		if (is_dynamic)
		{
			actor = PxCreateDynamic(*scene.m_system->getPhysics(),
									transform,
									*resource->getGeometry(),
									*scene.m_default_material,
									1.0f);
		}
		else
		{
			actor = PxCreateStatic(*scene.m_system->getPhysics(),
								   transform,
								   *resource->getGeometry(),
								   *scene.m_default_material);
		}
		if (actor)
		{
			setPhysxActor(actor);
		}
		else
		{
			g_log_error.log("Physics") << "Could not create PhysX mesh "
									 << resource->getPath().c_str();
		}
	}
}


void PhysicsSceneImpl::RigidActor::setPhysxActor(physx::PxRigidActor* actor)
{
	if (physx_actor)
	{
		scene.m_scene->removeActor(*physx_actor);
		physx_actor->release();
	}
	physx_actor = actor;
	if (actor)
	{
		scene.m_scene->addActor(*actor);
		actor->userData = (void*)(intptr_t)entity.index;
		scene.updateFilterData(actor, layer);
	}
}


void PhysicsSceneImpl::RigidActor::setResource(PhysicsGeometry* _resource)
{
	if (resource)
	{
		resource->getObserverCb().unbind<RigidActor, &RigidActor::onStateChanged>(this);
		resource->getResourceManager().get(PHYSICS_HASH)->unload(*resource);
	}
	resource = _resource;
	if (resource)
	{
		resource->onLoaded<RigidActor, &RigidActor::onStateChanged>(this);
	}
}


Heightfield::Heightfield()
{
	m_heightmap = nullptr;
	m_xz_scale = 1.0f;
	m_y_scale = 1.0f;
	m_actor = nullptr;
	m_layer = 0;
}


Heightfield::~Heightfield()
{
	if (m_heightmap)
	{
		m_heightmap->getResourceManager()
			.get(TEXTURE_HASH)
			->unload(*m_heightmap);
		m_heightmap->getObserverCb().unbind<Heightfield, &Heightfield::heightmapLoaded>(
			this);
	}
}


void Heightfield::heightmapLoaded(Resource::State, Resource::State new_state)
{
	if (new_state == Resource::State::READY)
	{
		m_scene->heightmapLoaded(this);
	}
}


void PhysicsScene::registerLuaAPI(lua_State* L)
{
	#define REGISTER_FUNCTION(name) \
		do {\
			auto f = &LuaWrapper::wrapMethod<PhysicsSceneImpl, decltype(&PhysicsSceneImpl::name), &PhysicsSceneImpl::name>; \
			LuaWrapper::createSystemFunction(L, "Physics", #name, f); \
		} while(false) \

	REGISTER_FUNCTION(getActorComponent);
	REGISTER_FUNCTION(putToSleep);
	REGISTER_FUNCTION(getActorSpeed);
	REGISTER_FUNCTION(applyForceToActor);
	REGISTER_FUNCTION(moveController);
	
	LuaWrapper::createSystemFunction(L, "Physics", "raycast", &PhysicsSceneImpl::LUA_raycast);

	#undef REGISTER_FUNCTION
}


} // namespace Lumix
