/*
 * Copyright (C) 2015-2019 Intel Corporation. All rights reserved.
 *
 * The information and source code contained herein is the exclusive
 * property of Intel Corporation and may not be disclosed, examined
 * or reproduced in whole or in part without explicit written authorization
 * from the company.
 */

#ifndef ACCEL_KNOBS_H
#define ACCEL_KNOBS_H

#include <algorithm>
#include <fstream>
#include <list>
#include <sstream>
#include <string>

namespace MEMTRACE
{

class KnobBase;

// Don't rearrage, code relies on enum order
enum KnobSection
{
    KNOB_SECTION_MEMTRACE,
    KNOB_SECTION_MEM,
    KNOB_SECTION_MAX,
};

// Don't rearrage, code relies on enum order
enum KnobSet
{
    KNOB_SET_RUNTIME, // Set dynamically at runtime
    KNOB_SET_USERENV, // Set by the user
    KNOB_SET_CONFIG,  // Set from the config file
    KNOB_SET_DEFAULT, // Set by the knob definition
    KNOB_SET_MAX,
};

class KnobMaster
{
  public:
    KnobMaster();
    void readConfig(std::string config);
    void readKnobEnvs();
    void setEnvs();
    KnobBase *knobDefined(const std::string &name, KnobSection _knobSection);
    void insertKnob(KnobBase *knob, KnobSection _knobSection);
    void dumpKnobs(std::string dumpFileName);

    std::list<KnobBase *> knobs_registry[KNOB_SECTION_MAX];
    KnobSection knobSection;
};

extern KnobMaster *p_km;
extern void processKnobs();

class KnobBase
{
  public:
    KnobSet knobSet;

    KnobBase()
    {
        knobSet = KNOB_SET_MAX;
    }

    /**
        * Get the original name of the knob
        *
        * @return The original name of the knob
        */
    virtual std::string name() = 0;

    /**
        * Get the upper-cased name of the knob. This is the name of the environment
        * variable that will be fetched.
        *
        * @return The name of the environment variable for the knob
        */
    virtual std::string upperName() const = 0;

    /**
        * Initialize the knob from the environment variable, if it is defined
        */
    virtual void set(const char *value) = 0;

    /**
        * Dump the knob to a file
        */
    virtual std::string dump(std::ofstream &out) = 0;

    virtual void setFromEnv() = 0;
    virtual void propagateToEnv() = 0;

}; // class KnobBase

/*
     *
     */
void registerKnob(KnobBase *knob);
// void checkForQuotes(const char *value);

/**
     * The Knob2 class handles getting environment variables. The environment
     * variable evaluation is delayed until the first use so that the application
     * can set them if it wants to.
     * This is named Knob2 to avoid a name collision with building the PDX flow.
     */
template <class T>
class Knob2 : public KnobBase
{
  public:
    std::string name_;       ///< The knob name
    std::string upper_name_; ///< The environment variable name
    T val_;                  ///< The knob value

    /**
        * Construct a Knob2 instance. Saves the environment variable name and
        * default value for later evaluation
        *
        * @param name - The environment variable name. Will be uppercased.
        * @param desc - Description. Not used for now.
        * @param def - Default value
        */
    Knob2(const std::string &name, const std::string &desc, const T &def) : KnobBase()
    {

        // Save the knob name and convert to uppercase for later reading
        // the environment variable version of the knob.
        name_ = name;
        upper_name_ = name;
        transform(upper_name_.begin(), upper_name_.end(), upper_name_.begin(), ::toupper);

        // Save the default value
        val_ = def;

        registerKnob(this);
    }

    /**
        * Get the raw knob name
        *
        * @return the knob name
        */
    std::string name() override
    {
        return name_;
    }

    /**
        * Get the knob name uppercased - This is the environment variable that will
        * be checked.
        *
        * @return the knob name
        */
    std::string upperName() const override
    {
        return upper_name_;
    }

    /**
         * Assignment operation - Allows the code to override any settings and explicitly
         * set the value
         *
         * @param x - The value to set val_ to
         * @return a reference to the knob
         */
    T &operator=(const T &x)
    {
        val_ = x;
        knobSet = KNOB_SET_RUNTIME;
        return val_;
    }

    /**
         * Call the target. Evaluates the environment variable if this hasn't
         * been done yet.
         *
         * @return the value of the knob
         */
    const T &operator()()
    {
        return *this;
    }

    /**
         * Call the target. Evaluates the environment variable if this hasn't
         * been done yet.
         *
         * @return the value of the knob
         */
    inline operator T()
    {
        return val_;
    }

    /**
        * Specialized method to set the knob value from the environment string.
        */
    void set(const char *envValue) override;

    /**
        * Dump the knob to a file. Note this will be specialized for
        * some types
        */
    std::string dump(std::ofstream &out) override
    {
        std::stringstream ss;
        ss << name() << "=" << val_;
        return ss.str();
    }

    void setFromEnv() override
    {
        const char *envValue = getenv(upper_name_.c_str());
        if (envValue)
        {
            // checkForQuotes(envValue);
            set(envValue);
        }
    }

    void propagateToEnv() override
    {
        std::ostringstream os;
        os << val_;
        std::string s = os.str();
        setenv(upperName().c_str(), s.c_str(), 1);
    }
}; // class Knobs2
} // namespace MEMTRACE

#endif // ACCEL_KNOBS_H
