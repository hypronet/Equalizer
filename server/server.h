
/* Copyright (c) 2005-2007, Stefan Eilemann <eile@equalizergraphics.com> 
   All rights reserved. */

#ifndef EQS_SERVER_H
#define EQS_SERVER_H

#include "base.h"

#include <eq/client/nodeType.h>  // for TYPE_EQ_SERVER enum
#include <eq/net/commandQueue.h> // member
#include <eq/net/idHash.h>       // member
#include <eq/net/node.h>         // base class

/** 
 * C entry point for the client library to start a local server within the
 * application process.
 * 
 * @return the local connection to the started server, or 0 when an error
 *         occured.
 */
extern "C" eqBase::RefPtr< eqNet::Connection > eqsStartLocalServer();

/** 
 * @namespace eqs
 * @brief The Equalizer server library.
 *
 * This namespace implements the server-side functionality for the Equalizer
 * framework.
 */
namespace eqs
{
    class Config;
    class Node;

    /**
     * The Equalizer server.
     */
    class EQS_EXPORT Server : public eqNet::Node
    {
    public:
        /** 
         * Constructs a new Server.
         */
        Server();

        /** 
         * Runs the server.
         * 
         * @return <code>true</code> if the server did run successfully,
         *         <code>false</code> if not.
         */
        bool run();

        /** 
         * Map a config to this server.
         * @todo 
         * @param config the config.
         */
        void mapConfig( Config* config );
        void unmapConfig( Config* config )
            { unmapSession( (eqNet::Session*)config ); }
        
        /** 
         * Add a new config to this server.
         * 
         * @param config the config.
         */
        void addConfig( Config* config );

        /** 
         * Remove a config from this config.
         * 
         * @param config the config
         * @return <code>true</code> if the config was removed,
         *         <code>false</code> otherwise.
         */
        bool removeConfig( Config* config );

        /** 
         * Return the number of configs on this config.
         * 
         * @return the number of configs on this config. 
         */
        uint32_t nConfigs() const { return _configs.size(); }

        /** 
         * Get a config.
         * 
         * @param index the config's index. 
         * @return the config.
         */
        Config* getConfig( const uint32_t index ) const 
            { return _configs[index]; }

    protected:
        virtual ~Server() {}

        /** @sa eqNet::Node::handleCommand */
        virtual eqNet::CommandResult handleCommand( eqNet::Command& command );
        
        /** @sa eqNet::Node::pushCommand */
        virtual bool pushCommand( eqNet::Command& command )
            { _commandQueue.push( command ); return true; }

    private:
        /** The unique config identifier. */
        uint32_t _configID;

        /** The list of nodes. */
        std::vector<Node*>     _nodes;

        /** The list of configurations. */
        std::vector<Config*>   _configs;

        /** The application-allocated configurations, mapped by identifier. */
        eqNet::IDHash<Config*> _appConfigs;

        /** The receiver->main command queue. */
        eqNet::CommandQueue    _commandQueue;

        /** @sa eqNet::Node::getType */
        virtual uint32_t getType() const { return eq::TYPE_EQ_SERVER; }

        void        _handleCommands(); 

        /** The command functions. */
        eqNet::CommandResult _reqChooseConfig( eqNet::Command& command );
        eqNet::CommandResult _reqUseConfig( eqNet::Command& command );
        eqNet::CommandResult _reqReleaseConfig( eqNet::Command& command );
    };

    std::ostream& operator << ( std::ostream& os, const Server* server );
};
#endif // EQS_SERVER_H
